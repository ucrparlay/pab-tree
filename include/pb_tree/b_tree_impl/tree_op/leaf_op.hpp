#ifndef PBTREE_B_TREE_IMPL_LEAF_OP_HPP_
#define PBTREE_B_TREE_IMPL_LEAF_OP_HPP_

#include <algorithm>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <numeric>
#include <tuple>
#include <type_traits>
#include <utility>

#include "pb_tree/b_tree.h"
#include "pb_tree/dependence/concepts.h"

namespace pb_tree {
template<typename TT>
template<typename Range>
TT::Node*
BTree<TT>::PostProcessLeafChange(Node* T,
                                 Range In,
                                 const TT::PatternType leaf_entries_pattern)
{
  // WARN: does not know whether the T->elements are sorted or not,
  // so might mark it as unsorted if the leaf entries pattern is unsort/hybrid,
  // even if the entries are actually sorted.
  assert(T->is_leaf);
  auto TL = static_cast<Leaf*>(T);
  if (leaf_entries_pattern == TT::LeafEntriesPattern::kSorted) {
    std::sort(TL->GetEleBeginIter(), TL->GetEleEndIter());
    TL->is_sorted = true;
  } else if (leaf_entries_pattern == TT::LeafEntriesPattern::kUnsorted) {
    TL->is_sorted = false;
  } else if (leaf_entries_pattern == TT::LeafEntriesPattern::kHybrid) {
    if (PseudoRandomGenPattern(T, In)) {
      std::sort(TL->GetEleBeginIter(), TL->GetEleEndIter());
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
BTree<TT>::ExtractPointsInLeaf(Node* T, Range Out)
{
  auto TL = static_cast<Leaf*>(T);
  if constexpr (sorted) {
    if (!TL->is_sorted) {
      std::sort(TL->elements.begin(), TL->elements.begin() + TL->size);
      TL->is_sorted = true;
    }
  }
  assert(Out.size() >= TL->size);
  std::ranges::copy_n(TL->GetEleBeginIter(), TL->size, Out.begin());
}

template<typename TT>
template<typename Range>
TT::Node*
BTree<TT>::InsertPoints2Leaf(Node* T,
                             Range In,
                             TT::PatternType leaf_entries_pattern)
{
  Leaf* TL = static_cast<Leaf*>(T);

  assert(T->size + In.size() <= TT::kBufUB);
  assert(T->size == TL->GetEle().size());
  if (TL->elements.size() == 0) {
    assert(TL->size == 0);
    TL->InitializeEleSeq();
    assert(TL->elements.size() == TT::kBufUB);
  }
  // PERF: BufSizeT should be enough since total size fits within
  std::ranges::copy_n(In.begin(), In.size(), TL->GetEleEndIter());
  TL->size += In.size();
  TL->UpdateAug(
    parlay::make_slice(TL->elements.begin(), TL->elements.begin() + TL->size));

  return PostProcessLeafChange(T, In, leaf_entries_pattern);
}

template<typename TT>
template<typename Range>
TT::SizeT
BTree<TT>::HandlingLeafSplitOverflow(Node* T,
                                     Range In,
                                     NodeSplitPairSlice node_split_slice)
{
  Leaf* TL = static_cast<Leaf*>(T);
  auto entry_seq = TT::EntrySeq::uninitialized(T->size + In.size());

  if (TL->is_sorted) {
    // In is always sorted (sorted by BatchInsert_ before descent).
    // Both halves sorted: O(n) merge instead of O(n log n) sort.
    std::ranges::merge(TL->GetEleBeginIter(),
                       TL->GetEleEndIter(),
                       In.begin(),
                       In.end(),
                       entry_seq.begin(),
                       [](E const& a, E const& b) {
                         return Entry::GetKey(a) < Entry::GetKey(b);
                       });
    FreeNode<Leaf>(T);
  } else {
    std::ranges::copy_n(TL->GetEleBeginIter(), TL->size, entry_seq.begin());
    parlay::copy(
      In, parlay::make_slice(entry_seq.begin() + TL->size, entry_seq.end()));
    FreeNode<Leaf>(T);
    entry_seq = parlay::sort(entry_seq);
  }

  SizeT rt_bkt_n = GenerateLeafNodes(
    parlay::make_slice(entry_seq),
    GenNodesOutputHandler{
      .set_key = [&](SizeT i, auto key) { node_split_slice[i].second = key; },
      .set_node = [&](SizeT i,
                      auto node) { node_split_slice[i].first = node; } });

  return rt_bkt_n;
}

template<typename TT>
TT::Node*
BTree<TT>::DeletePoints4Leaf(Node* T,
                             TT::KeySlice In,
                             const TT::PatternType leaf_entries_pattern)
{
  auto TL = static_cast<Leaf*>(T);

  assert(T->size >= In.size());
  assert(parlay::all_of(In, [&](auto a) {
    return std::find_if(TL->elements.begin(),
                        TL->elements.begin() + TL->size,
                        [&](auto const e) { return Entry::GetKey(e) == a; }) !=
           TL->elements.begin() + TL->size;
  }));

  auto it = TL->elements.begin();
  auto end = TL->elements.begin() + TL->size;
  BufSizeT delete_cnt = 0;
  for (SizeT i = 0; i < In.size(); i++) {
    it = std::ranges::find_if(TL->elements.begin(), end, [&](auto const& e) {
      return Entry::GetKey(e) == In[i];
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
  TL->UpdateAug(
    parlay::make_slice(TL->elements.begin(), TL->elements.begin() + TL->size));

  return PostProcessLeafChange(T, In, leaf_entries_pattern);
}

} // namespace pb_tree

#endif // PBTREE_B_TREE_IMPL_LEAF_OP_HPP_

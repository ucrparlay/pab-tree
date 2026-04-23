#ifndef PMOS_B_TREE_IMPL_LEAF_OP_HPP_
#define PMOS_B_TREE_IMPL_LEAF_OP_HPP_

#include <algorithm>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <numeric>
#include <tuple>
#include <type_traits>
#include <utility>

#include "pmos/b_tree.h"
#include "pmos/dependence/concepts.h"

namespace pmos {
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
  // for (BufSizeT i = 0; i < TL->size; i++) {
  //   Out[i] = TL->elements[i];
  // }
}

template<typename TT>
template<typename Range>
TT::Node*
BTree<TT>::InsertPoints2Leaf(Node* T, Range In)
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
  // for (BufSizeT i = 0; i < In.size(); i++) {
  //   TL->elements[TL->size + i] = In[i];
  // }
  TL->size += In.size();
  TL->is_sorted = false;

  // TL->UpdateAug(TL->elements.cut(0, TL->size));
  TL->UpdateAug(
    parlay::make_slice(TL->elements.begin(), TL->elements.begin() + TL->size));
  return T;
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
TT::SizeT
BTree<TT>::DeletePoints4Leaf(Node* T, TT::KeySlice In)
{
  auto TL = static_cast<Leaf*>(T);
  assert(T->size >= In.size());
  assert(parlay::all_of(In, [&](auto a) {
    return std::ranges::find_if(TL->elements.begin(),
                                TL->elements.begin() + TL->size,
                                [&](auto const e) {
                                  return Entry::GetKey(e) == a;
                                }) != TL->elements.begin() + TL->size;
  }));

  auto* begin = TL->GetEleBeginIter();
  const BufSizeT m = static_cast<BufSizeT>(TL->size);

  // Sort leaf if unsorted — In is always sorted (sorted once in BatchDelete_).
  // O(m log m) sort pays off against the O(n*m) linear scan whenever n > log m.
  if (!TL->is_sorted) {
    std::sort(begin, begin + m, [](E const& a, E const& b) {
      return Entry::GetKey(a) < Entry::GetKey(b);
    });
    TL->is_sorted = true;
  }

  // Two-pointer O(m + n) compaction: walk sorted leaf and sorted In together.
  BufSizeT write = 0;
  BufSizeT j = 0;
  const BufSizeT n = static_cast<BufSizeT>(In.size());
  for (BufSizeT i = 0; i < m; ++i) {
    if (j < n && Entry::GetKey(begin[i]) == In[j]) {
      ++j; // delete: skip this element
    } else {
      if (write != i) {
        begin[write] = begin[i];
      }
      ++write;
    }
  }

  assert(j == n); // all keys in In were found and deleted
  TL->size = write;
  // is_sorted stays true: surviving elements are a sorted subsequence
  TL->UpdateAug(
    parlay::make_slice(TL->elements.begin(), TL->elements.begin() + TL->size));
  return static_cast<SizeT>(j);
}
} // namespace pmos

#endif // PMOS_B_TREE_IMPL_LEAF_OP_HPP_

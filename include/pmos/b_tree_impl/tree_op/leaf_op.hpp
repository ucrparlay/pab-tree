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
  for (BufSizeT i = 0; i < TL->size; i++) {
    Out[i] = TL->elements[i];
  }
}

template<typename TT>
TT::Node*
BTree<TT>::InsertPoints2Leaf(Node* T, EntrySlice In)
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
TT::SizeT
BTree<TT>::HandlingLeafSplitOverflow(Node* T,
                                     EntrySlice In,
                                     NodeSplitPairSlice node_split_slice)
{
  auto entry_seq = TT::EntrySeq::uninitialized(T->size + In.size());
  Leaf* TL = static_cast<Leaf*>(T);

  // for (SizeT i = 0; i < T->size; i++) { // Assume the TT::kBufUB is constant
  //   entry_seq[i] = TL->elements[i];
  // }
  std::ranges::copy_n(TL->GetEleBeginIter(), TL->size, entry_seq.begin());
  parlay::copy(
    In, parlay::make_slice(entry_seq.begin() + TL->size, entry_seq.end()));
  // parlay::parallel_for(
  //   0, In.size(), [&](SizeT i) { entry_seq[T->size + i] = In[i]; });
  FreeNode<Leaf>(T);

  // NOTE: split
  entry_seq = parlay::sort(entry_seq); // PERF: we need sort as In is unsorted

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
    return std::find_if(TL->elements.begin(),
                        TL->elements.begin() + TL->size,
                        [&](auto const e) { return Entry::GetKey(e) == a; }) !=
           TL->elements.begin() + TL->size;
  }));

  auto it = TL->elements.begin();
  auto end = TL->elements.begin() + TL->size;
  BufSizeT delete_cnt = 0;
  for (SizeT i = 0; i < In.size(); i++) {
    it = std::ranges::find_if(TL->elements.begin(), end, [&](auto const e) {
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
  TL->is_sorted = false;
  assert(TL->size >= 0);
  // TL->UpdateAug(TL->elements.cut(0, TL->size));
  TL->UpdateAug(
    parlay::make_slice(TL->elements.begin(), TL->elements.begin() + TL->size));
  return delete_cnt;
}
} // namespace pmos

#endif // PMOS_B_TREE_IMPL_LEAF_OP_HPP_

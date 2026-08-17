#ifndef PBTREE_B_TREE_IMPL_QUERY_OP_RANGE_COUNT_HPP_
#define PBTREE_B_TREE_IMPL_QUERY_OP_RANGE_COUNT_HPP_

#include "dependence/binary_search.h"
#include "parlay/slice.h"
#include "pb_tree/b_tree.h"

namespace pb_tree {

template<typename TT>
auto
BTree<TT>::RangeCount(TT::Box const query_box) const
  -> std::pair<SizeT, RangeReportLogger>
{
  RangeReportLogger logger;
  SizeT rt_size = 0;
  if (this->root_ == nullptr) [[unlikely]] {
    return { 0, logger };
  }
  RangeCountRec(this->root_, query_box, rt_size, logger);
  return { rt_size, logger };
}

template<typename TT>
void
BTree<TT>::RangeCountLeaf(Node const* T,
                          TT::Box const& query_box,
                          SizeT& offset) const
{
  auto const* TL = static_cast<Leaf const*>(T);
  if (TL->is_sorted) {
    const auto l = TL->BinarySearchKey(query_box.first);
    // Narrow: search for r only in [l, size) since r >= l.
    const auto* elems = TL->elements.data() + l;
    const BufSizeT rem = static_cast<BufSizeT>(TL->size) - static_cast<BufSizeT>(l);
    size_t lo = 0, hi = static_cast<size_t>(rem);
    while (hi - lo > 8) {
      size_t const mid = lo + ((hi - lo) >> 1);
      bool   const go  = Entry::GetKey(elems[mid]) < query_box.second;
      lo = go ? mid + 1 : lo;
      hi = go ? hi      : mid;
    }
    size_t cnt = 0;
    for (size_t i = lo; i < hi; ++i) {
      cnt += (Entry::GetKey(elems[i]) < query_box.second) ? 1 : 0;
    }
    const auto r = l + static_cast<decltype(l)>(lo + cnt);
    assert(l <= r && r <= TL->size);
    offset += r - l;
    return;
  }

  // Unsorted leaf: branchless count
  SizeT cnt = 0;
  for (BufSizeT i = 0; i < static_cast<BufSizeT>(TL->size); i++) {
    auto const k = Entry::GetKey(TL->elements[i]);
    cnt += (k >= query_box.first && k < query_box.second) ? 1 : 0;
  }
  offset += cnt;
}

template<typename TT>
void
BTree<TT>::RangeCountRec(Node const* T,
                         TT::Box const& query_box,
                         SizeT& offset,
                         RangeReportLogger& logger) const
{
  // Iterative descent for single-child levels (avoids O(height) call overhead).
  while (!T->is_leaf) {
    logger.vis_interior_num++;
    auto const* TI = static_cast<Interior const*>(T);

    // Left boundary child
    const BufSizeT l = TI->GetNodeIdContainingKey(query_box.first);

    // Right boundary: narrow search to [l, split_n) since r >= l.
    const BufSizeT split_n = TI->GetSplitN();
    const Split*   sp_l    = TI->split.data() + l;
    const size_t   rem     = static_cast<size_t>(split_n) - static_cast<size_t>(l);
    size_t lo = 0, hi = rem;
    while (hi - lo > 8) {
      size_t const mid = lo + ((hi - lo) >> 1);
      bool   const go  = (sp_l[mid] < query_box.second);
      lo = go ? mid + 1 : lo;
      hi = go ? hi      : mid;
    }
    size_t cnt = 0;
    for (size_t i = lo; i < hi; ++i) {
      cnt += (sp_l[i] < query_box.second) ? 1 : 0;
    }
    const BufSizeT r = l + static_cast<BufSizeT>(lo + cnt);

    assert(l < TI->GetNodeN());
    assert(r < TI->GetNodeN());
    assert(TI->ValidateKeyInNode(l, query_box.first));
    assert(TI->ValidateKeyInNode(r, query_box.second) ||
           TI->split[r] == query_box.second);

    if (l == r) {
      T = TI->tree_nodes[l];
      continue;
    }

    RangeCountRec(TI->tree_nodes[l], query_box, offset, logger);

    // Middle children: just sum their sizes (no traversal needed).
    for (BufSizeT i = l + 1; i < r; i++) {
      offset += TI->tree_nodes[i]->size;
    }

    RangeCountRec(TI->tree_nodes[r], query_box, offset, logger);
    return;
  }

  logger.vis_leaf_num++;
  RangeCountLeaf(T, query_box, offset);
}

} // namespace pb_tree

#endif // PBTREE_B_TREE_IMPL_QUERY_OP_RANGE_COUNT_HPP_

#ifndef PMOS_B_TREE_IMPL_QUERY_OP_RANGE_REPORT_HPP_
#define PMOS_B_TREE_IMPL_QUERY_OP_RANGE_REPORT_HPP_

#include <algorithm>

#include "dependence/binary_search.h"
#include "parlay/parallel.h"
#include "parlay/slice.h"
#include "pmos/b_tree.h"

namespace pmos {

template<typename TT>
template<typename Range>
auto
BTree<TT>::RangeReport(TT::Box const query_box, Range&& out) const
  -> std::pair<SizeT, RangeReportLogger>
{
  if (query_box.first >= query_box.second) [[unlikely]] {
    return { 0, RangeReportLogger{} };
  }

  RangeReportLogger logger;
  SizeT rt_size = 0;
  RangeReportRec(
    this->root_, query_box, rt_size, parlay::make_slice(out), logger);
  return { rt_size, logger };
}

template<typename TT>
template<typename Range>
void
BTree<TT>::RangeReportLeaf(Node const* T,
                           TT::Box const& query_box,
                           SizeT& offset,
                           Range out) const
{
  auto const* TL = static_cast<Leaf const*>(T);
  if (TL->is_sorted) {
    const auto l = TL->BinarySearchKey(query_box.first);
    // Narrow: search for r only in [l, size) since r >= l.
    const auto* elems = TL->elements.data() + l;
    const BufSizeT rem =
      static_cast<BufSizeT>(TL->size) - static_cast<BufSizeT>(l);
    size_t lo = 0, hi = static_cast<size_t>(rem);
    while (hi - lo > 8) {
      size_t const mid = lo + ((hi - lo) >> 1);
      bool const go = Entry::GetKey(elems[mid]) < query_box.second;
      lo = go ? mid + 1 : lo;
      hi = go ? hi : mid;
    }
    size_t cnt = 0;
    for (size_t i = lo; i < hi; ++i) {
      cnt += (Entry::GetKey(elems[i]) < query_box.second) ? 1 : 0;
    }
    const auto r = l + static_cast<decltype(l)>(lo + cnt);
    assert(l <= r && r <= TL->size);
    std::copy_n(TL->elements.begin() + l, r - l, out.begin() + offset);
    offset += r - l;
    return;
  }

  // Unsorted leaf: linear scan
  for (BufSizeT i = 0; i < static_cast<BufSizeT>(TL->size); i++) {
    auto const k = Entry::GetKey(TL->elements[i]);
    if (k >= query_box.first && k < query_box.second) {
      out[offset++] = TL->elements[i];
    }
  }
}

template<typename TT>
template<typename Range>
void
BTree<TT>::RangeReportRec(Node const* T,
                          TT::Box const& query_box,
                          SizeT& offset,
                          Range out,
                          RangeReportLogger& logger) const
{
  // Iterative descent: avoids O(height) recursive-call overhead for narrow
  // queries that fall inside a single child at each interior level.
  while (!T->is_leaf) {
    logger.vis_interior_num++;
    auto const* TI = static_cast<Interior const*>(T);
    auto* const* nodes =
      TI->tree_nodes.data(); // Node* const* → nodes[i] is Node*

    // Left boundary child (first child overlapping query_box.first)
    const BufSizeT l = TI->GetNodeIdContainingKey(query_box.first);

    // Right boundary child: narrow search to [l, split_n) since r >= l,
    // saving roughly half the binary-search work in the common case.
    const BufSizeT split_n = TI->GetSplitN();
    const Split* sp_l = TI->split.data() + l;
    const size_t rem = static_cast<size_t>(split_n) - static_cast<size_t>(l);
    size_t lo = 0, hi = rem;
    while (hi - lo > 8) {
      size_t const mid = lo + ((hi - lo) >> 1);
      bool const go = (sp_l[mid] < query_box.second);
      lo = go ? mid + 1 : lo;
      hi = go ? hi : mid;
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
      // Entire query falls in one child — keep descending without recursion.
      T = nodes[l];
      continue;
    }

    // Query spans children [l, r].

    // Left boundary: partially covered, recurse.
    RangeReportRec(nodes[l], query_box, offset, out, logger);

    // Middle children [l+1, r): fully covered by the query.
    // Pre-compute each child's output position, then flatten in parallel.
    if (r > l + 1) {
      const BufSizeT mid_n = r - l - 1;
      SizeBufUBArr off; // SizeBufUBArr = std::array<SizeT, kBufUB>
      off[0] = offset;
      for (BufSizeT k = 0; k < mid_n; ++k) {
        off[k + 1] = off[k] + nodes[l + 1 + k]->size;
      }
      offset = off[mid_n];
      parlay::parallel_for(
        0,
        mid_n,
        [&](BufSizeT k) {
          FlattenRec<Range, true, false>(nodes[l + 1 + k],
                                         out.cut(off[k], off[k + 1]));
        },
        1);
    }

    // Right boundary: partially covered, recurse.
    RangeReportRec(nodes[r], query_box, offset, out, logger);
    return;
  }

  // T is a leaf.
  logger.vis_leaf_num++;
  RangeReportLeaf(T, query_box, offset, out);
}

// Sequential full-subtree flatten used when only a logger-tracked path is
// needed (kept for callers that pass a RangeReportLogger).
template<typename TT>
template<typename Range>
void
BTree<TT>::RangeReportFlattenRec(Node const* T,
                                 SizeT& offset,
                                 Range out,
                                 RangeReportLogger& logger) const
{
  if (T->is_leaf) {
    logger.flatten_node_num++;
    auto const* TL = static_cast<Leaf const*>(T);
    std::copy_n(TL->elements.begin(), TL->size, out.begin() + offset);
    offset += TL->size;
    return;
  }

  auto const* TI = static_cast<Interior const*>(T);
  const BufSizeT node_n = TI->GetNodeN();
  for (BufSizeT i = 0; i < node_n; i++) {
    RangeReportFlattenRec(TI->tree_nodes[i], offset, out, logger);
  }
}

} // namespace pmos

#endif // PMOS_B_TREE_IMPL_QUERY_OP_RANGE_REPORT_HPP_

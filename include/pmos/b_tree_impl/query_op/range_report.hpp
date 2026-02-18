#ifndef PMOS_B_TREE_IMPL_QUERY_OP_RANGE_REPORT_HPP_
#define PMOS_B_TREE_IMPL_QUERY_OP_RANGE_REPORT_HPP_

#include "dependence/binary_search.h"
#include "parlay/slice.h"
#include "pmos/b_tree.h"

namespace pmos {
template<typename TT>
template<typename Range>
auto
BTree<TT>::RangeReport(TT::Box const query_box, Range&& out) const
  -> std::pair<SizeT, RangeReportLogger>
{
  RangeReportLogger logger;
  return { RangeReportRec(this->root_, query_box, std::forward<Range>(out)),
           logger };
}

template<typename TT>
template<typename Range>
TT::BufSizeT
BTree<TT>::RangeReportLeaf(Node const* T, TT::Box query_box, Range out) const
{
  auto TL = static_cast<Leaf const*>(T);
  if (TL->is_sorted) {
    auto l = TL->BinarySearchKey(query_box.first);
    auto r = TL->BinarySearchKey(query_box.second);
    assert(l <= r);
    assert(r <= TL->size);
    for (BufSizeT i = l; i < r; i++) {
      out[i] = TL->elements[i];
    }
    return r - l;
  }

  BufSizeT cnt = 0;
  for (BufSizeT i = 0; i < TL->size; i++) {
    auto k = Entry::GetKey(TL->elements[i]);
    if (k >= query_box.first && k <= query_box.second) {
      out[cnt++] = TL->elements[i];
    }
  }
  return cnt;
}

template<typename TT>
template<typename Range>
TT::SizeT
BTree<TT>::RangeReportRec(Node const* T,
                          TT::Box const query_box,
                          Range out) const
{
  if (T->is_leaf) {
    return RangeReportLeaf(T, query_box, out);
  }

  auto const* TI = static_cast<Interior const*>(T);
  auto l = TI->GetNodeIdContainingKey(query_box.first);
  // BUG: double check the left close and right open
  auto r = TI->GetNodeIdContainingKeyExclusive(query_box.second);

  assert(l < TI->GetNodeN());
  assert(r < TI->GetNodeN());
  assert(TI->ValidateKeyInNode(l, query_box.first));
  assert(TI->ValidateKeyInNode(r, query_box.second));

  if (l == r) { // falls into one node
    return RangeReportRec(TI->tree_nodes[l], query_box, out);
  }

  if (l + 1 == r) { // two single node
    auto count_l = RangeReportRec(TI->tree_nodes[l], query_box, out);
    auto count_r = RangeReportRec(TI->tree_nodes[r], query_box, out);
    return count_l + count_r;
  }

  // contain nodes that can be flattened
  auto count_l = RangeReportRec(TI->tree_nodes[l], query_box, out);
  parlay::parallel_for(l + 1, r, [&](BufSizeT i) {
    SizeT sum = count_l;
    for (BufSizeT j = l + 1; j < i; j++) {
      sum += TI->tree_nodes[j]->size;
    }
    FlattenRec(TI->tree_nodes[i], out.cut(sum, sum + TI->tree_nodes[i]->size));
  });
  count_l +=
    std::accumulate(TI->tree_nodes.begin() + l + 1,
                    TI->tree_nodes.begin() + r,
                    SizeT(0),
                    [](SizeT acc, Node* node) { return acc + node->size; });
  auto count_r =
    RangeReportRec(TI->tree_nodes[r],
                   query_box,
                   out.cut(count_l, count_l + TI->tree_nodes[r]->size));
  return count_l + count_r;
}

} // namespace pmos

#endif // PMOS_B_TREE_IMPL_QUERY_OP_RANGE_REPORT_HPP_

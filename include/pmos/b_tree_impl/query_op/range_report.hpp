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
  // the bounding box should be [a, b)
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
                           TT::Box query_box,
                           SizeT& offset,
                           Range out) const
{
  auto TL = static_cast<Leaf const*>(T);
  if (TL->is_sorted) {
    auto l = TL->BinarySearchKey(query_box.first);
    auto r = TL->BinarySearchKey(query_box.second);
    assert(l <= r);
    assert(r <= TL->size);
    for (BufSizeT i = l; i < r; i++) {
      auto k = Entry::GetKey(TL->elements[i]);
      assert(k >= query_box.first && k < query_box.second);
      out[offset + i - l] = TL->elements[i];
      offset++;
    }
    return;
  }

  BufSizeT cnt = 0;
  for (BufSizeT i = 0; i < TL->size; i++) {
    auto k = Entry::GetKey(TL->elements[i]);
    if (k >= query_box.first && k < query_box.second) {
      out[offset++] = TL->elements[i];
    }
  }
}

template<typename TT>
template<typename Range>
void
BTree<TT>::RangeReportRec(Node const* T,
                          TT::Box const query_box,
                          SizeT& offset,
                          Range out,
                          RangeReportLogger& logger) const
{
  if (T->is_leaf) {
    logger.vis_leaf_num++;
    RangeReportLeaf(T, query_box, offset, out);
    return;
  }

  logger.vis_interior_num++;
  auto const* TI = static_cast<Interior const*>(T);
  auto l = TI->GetNodeIdContainingKey(query_box.first);
  auto r = TI->GetNodeIdContainingKeyExclusive(query_box.second);

  assert(l < TI->GetNodeN());
  assert(r < TI->GetNodeN());
  assert(TI->ValidateKeyInNode(l, query_box.first));
  assert(TI->ValidateKeyInNode(r, query_box.second) ||
         TI->split[r] == query_box.second);

  // NOTE: visit nodes [l, r]
  // for (BufSizeT i = l; i <= r; i++) {
  //   RangeReportRec(TI->tree_nodes[i], query_box, offset, out, logger);
  // }
  // return;

  if (l == r) { // falls into one node
    RangeReportRec(TI->tree_nodes[l], query_box, offset, out, logger);
    return;
  }

  RangeReportRec(TI->tree_nodes[l], query_box, offset, out, logger);
  RangeReportRec(TI->tree_nodes[r], query_box, offset, out, logger);

  if (r > l + 1) {
    for (BufSizeT i = l + 1; i < r; i++) {
      FlattenRec<decltype(out), false>(
        TI->tree_nodes[i], out.cut(offset, offset + TI->tree_nodes[i]->size));
      offset += TI->tree_nodes[i]->size;
    }
    // parlay::parallel_for(l + 1, r, [&](BufSizeT i) {
    //   SizeT start = offset;
    //   for (BufSizeT j = l + 1; j < i; j++) {
    //     start += TI->tree_nodes[j]->size;
    //   }
    //   FlattenRec<decltype(out), false>(
    //     TI->tree_nodes[i], out.cut(start, start + TI->tree_nodes[i]->size));
    // });
  }
  // auto count_l = RangeReportRec(TI->tree_nodes[l], query_box, out);
  // parlay::parallel_for(l + 1, r, [&](BufSizeT i) {
  //   SizeT sum = count_l;
  //   for (BufSizeT j = l + 1; j < i; j++) {
  //     sum += TI->tree_nodes[j]->size;
  //   }
  //   assert(out.size() >= sum + TI->tree_nodes[i]->size);
  //   FlattenRec<decltype(out), false>(
  //     TI->tree_nodes[i], out.cut(sum, sum + TI->tree_nodes[i]->size));
  // });
  // count_l +=
  //   std::accumulate(TI->tree_nodes.begin() + l + 1,
  //                   TI->tree_nodes.begin() + r,
  //                   SizeT(0),
  //                   [](SizeT acc, Node* node) { return acc + node->size; });
  // for (BufSizeT i = l + 1; i < r; i++) {
  //   assert(out.size() >= count_l + TI->tree_nodes[i]->size);
  //   FlattenRec<decltype(out), false>(
  //     TI->tree_nodes[i], out.cut(count_l, count_l +
  //     TI->tree_nodes[i]->size));
  //   count_l += TI->tree_nodes[i]->size;
  // }
  // auto count_r =
  //   RangeReportRec(TI->tree_nodes[r], query_box, out.cut(count_l,
  //   out.size()));
  // return count_l + count_r;
}

} // namespace pmos

#endif // PMOS_B_TREE_IMPL_QUERY_OP_RANGE_REPORT_HPP_

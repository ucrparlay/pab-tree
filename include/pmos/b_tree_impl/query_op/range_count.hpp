#ifndef PMOS_B_TREE_IMPL_QUERY_OP_RANGE_COUNT_HPP_
#define PMOS_B_TREE_IMPL_QUERY_OP_RANGE_COUNT_HPP_

#include "dependence/binary_search.h"
#include "parlay/slice.h"
#include "pmos/b_tree.h"

namespace pmos {
template<typename TT>
auto
BTree<TT>::RangeCount(TT::Box const query_box) const
  -> std::pair<SizeT, RangeReportLogger>
{
  // the bounding box should be [a, b)
  RangeReportLogger logger;
  SizeT rt_size = 0;
  RangeCountRec(this->root_, query_box, rt_size, logger);
  return { rt_size, logger };
}

template<typename TT>
void
BTree<TT>::RangeCountLeaf(Node const* T,
                          TT::Box const& query_box,
                          SizeT& offset) const
{
  auto TL = static_cast<Leaf const*>(T);
  if (TL->is_sorted) {
    auto l = TL->BinarySearchKey(query_box.first);
    auto r = TL->BinarySearchKey(query_box.second);
    assert(l <= r);
    assert(r <= TL->size);
    offset += r - l;
    return;
  }

  BufSizeT cnt = 0;
  for (BufSizeT i = 0; i < TL->size; i++) {
    auto k = Entry::GetKey(TL->elements[i]);
    if (k >= query_box.first && k < query_box.second) {
      offset++;
    }
  }
}

template<typename TT>
void
BTree<TT>::RangeCountRec(Node const* T,
                         TT::Box const& query_box,
                         SizeT& offset,
                         RangeReportLogger& logger) const
{
  if (T->is_leaf) {
    logger.vis_leaf_num++;
    RangeCountLeaf(T, query_box, offset);
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
  if (l == r) { // falls into one node
    RangeCountRec(TI->tree_nodes[l], query_box, offset, logger);
    return;
  }

  RangeCountRec(TI->tree_nodes[l], query_box, offset, logger);
  RangeCountRec(TI->tree_nodes[r], query_box, offset, logger);

  if (r > l + 1) {
    for (BufSizeT i = l + 1; i < r; i++) {
      offset += TI->tree_nodes[i]->size;
    }
  }
}

} // namespace pmos

#endif // PMOS_B_TREE_IMPL_QUERY_OP_RANGE_COUNT_HPP_

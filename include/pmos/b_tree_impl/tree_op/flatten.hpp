#ifndef PMOS_B_TREE_IMPL_TREE_OP_FLATTEN_HPP_
#define PMOS_B_TREE_IMPL_TREE_OP_FLATTEN_HPP_

#include <algorithm>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <numeric>
#include <tuple>
#include <type_traits>
#include <utility>

#include "pmos/b_tree.h"

namespace pmos {

template<typename TT>
template<bool granularity>
inline bool
BTree<TT>::ForceParallelRecursion(Interior const* TI)
{
  // NOTE: return true -> force parallel
  // return false -> serial
  return (granularity && TI->size > kSerialBuildCutoff) ||
         (!granularity && TI->ForceParallel());
}

template<typename TT>
template<typename Range, bool granularity, bool sorted>
void
BTree<TT>::FlattenRec(Node* T, Range Out)
{
  assert(T->size == Out.size());

  if (T->size == 0) {
    return;
  }

  if (T->is_leaf) {
    ExtractPointsInLeaf<Range, sorted>(T, Out);
    return;
  }

  auto TI = static_cast<Interior*>(T);

  assert(TI->size == std::accumulate(TI->tree_nodes.begin(),
                                     TI->tree_nodes.begin() + TI->GetNodeN(),
                                     static_cast<size_t>(0),
                                     [](size_t acc, Node* n) -> size_t {
                                       return acc + n->size;
                                     }));

  std::array<SizeT, TT::kBufUB + 1> child_offsets;
  child_offsets[0] = 0;
  auto node_n = TI->GetNodeN();
  for (BufSizeT i = 0; i < node_n; ++i) {
    child_offsets[i + 1] = child_offsets[i] + TI->tree_nodes[i]->size;
  }

  parlay::parallel_for(
    0,
    node_n,
    [&](BufSizeT i) {
      auto start = child_offsets[i];
      FlattenRec<Range, granularity, sorted>(
        TI->tree_nodes[i], Out.cut(start, child_offsets[i + 1]));
    },
    // NOTE: In our btree when we flatten the tree, the size is always match the
    // corresponding size, so we can use it as the granularity indicator
    ForceParallelRecursion<granularity>(TI) ? 1 : Interior::TT::kBufUB);
}

} // namespace pmos

#endif // PMOS_B_TREE_IMPL_TREE_OP_FLATTEN_HPP_

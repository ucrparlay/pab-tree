#ifndef PMOS_B_TREE_IMPL_BATCH_OP_B_BUILD_TREE_HPP_
#define PMOS_B_TREE_IMPL_BATCH_OP_B_BUILD_TREE_HPP_

#include <parlay/range.h>
#include <parlay/slice.h>
#include <parlay/type_traits.h>

#include "pmos/b_tree.h"
#include "pmos/dependence/tree_node.h"

namespace pmos {
template <typename TT>
template <typename Range>
void BTree<TT>::Build(Range&& In) {
  static_assert(parlay::is_random_access_range_v<Range>);
  static_assert(
      parlay::is_less_than_comparable_v<parlay::range_reference_type_t<Range>>);
  static_assert(std::is_constructible_v<parlay::range_value_type_t<Range>,
                                        parlay::range_reference_type_t<Range>>);

  Slice A = parlay::make_slice(In);
  Build_(A);
}

template <typename TT>
void BTree<TT>::Build_(Slice In) {
  // this->root_ = BuildRecursive(A);
  auto sort_seq = parlay::sort(In);
  this->root_ = BuildRecursive(parlay::make_slice(sort_seq));
  assert(this->root_ != nullptr);
  return;
}

template <typename TT>
Node* BTree<TT>::BuildRecursive(Slice In) {
  size_t n = In.size();

  assert(n >= kBufLB || n == 0);

  if (n == 0) {
    return AllocEmptyLeafNode<Slice, Leaf>(In);
  }

  if (kBufLB <= n && n <= kBufUB) {
    return AllocNormalLeafNode<Slice, Leaf>(In);
  }

  auto tree_nodes = NodeSeq::uninitialized(TT::kBufUB);
  auto split_seq = SplitSeq::uninitialized(TT::kBufUB - 1);
  // BUG: what happens if n = 8 and B = 7?
  parlay::parallel_for(
      0, TT::kBufMD,
      [&](size_t i) {
        size_t start = i * (n / TT::kBufMD);
        size_t end = (i + 1) * (n / TT::kBufMD);
        // WARN: no handling of the duplicates here
        if (i == TT::kBufMD - 1) {
          end = n;
        } else {
          split_seq[i] = TT::GetKey(In[end]);
        }
        tree_nodes[i] = BuildRecursive(In.cut(start, end));
      },
      n >= kSerialBuildCutoff);
  return AllocInteriorNode<Interior>(tree_nodes, split_seq);
}

}  // namespace pmos

#endif  // PMOS_B_TREE_IMPL_BATCH_OP_B_BUILD_TREE_HPP_

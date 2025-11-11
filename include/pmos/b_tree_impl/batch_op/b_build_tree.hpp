#ifndef PMOS_B_TREE_IMPL_BATCH_OP_B_BUILD_TREE_HPP_
#define PMOS_B_TREE_IMPL_BATCH_OP_B_BUILD_TREE_HPP_

#include <parlay/range.h>
#include <parlay/slice.h>
#include <parlay/type_traits.h>

#include "b_tree.h"
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
  assert(this->root_ != nullptr);
  return;
}

template <typename TT>
Node* BTree<TT>::BuildRecursive(Slice In) {
  // if (In.size()) {
  if (In.size() <= BT::kSerialBuildCutoff) {
    return SerialBuildRecursive(In, Out, dim, bx);
  }

  auto pivots = SplitterSeq::uninitialized(BT::kPivotNum + BT::kBucketNum + 1);
  auto box_seq = BoxSeq::uninitialized(BT::kBucketNum);
  parlay::sequence<BallsType> sums;

  PickPivots(In, In.size(), pivots, dim, box_seq, bx);
  BT::Partition(In, Out, In.size(), pivots, sums);

  // NOTE: if random sampling failed to split points, re-partitions using
  // serail approach
  auto tree_nodes = parlay::sequence<Node*>::uninitialized(BT::kBucketNum);
  auto nodes_map = BucketSeq::uninitialized(BT::kBucketNum);
  BucketType zeros = std::ranges::count(sums, 0), cnt = 0;

  if (zeros == BT::kBucketNum - 1) {  // NOTE: switch to seral
    // TODO: add parallelsim within this call
    // see parallel kth element
    return SerialBuildRecursive(In, Out, dim, bx);
  }

  // NOTE: alloc empty leaf beforehand to avoid spawn threads
  for (BucketType i = 0; i < BT::kBucketNum; ++i) {
    if (!sums[i]) {
      tree_nodes[i] = AllocEmptyLeafNode<Slice, Leaf>();
    } else {
      nodes_map[cnt++] = i;
    }
  }

  for (BucketType i = 0; i < BT::kBuildDepthOnce; ++i) {
    dim = split_rule_.NextDimension(dim);
  }

  parlay::parallel_for(
      0, BT::kBucketNum - zeros,
      [&](BucketType i) {
        size_t start = 0;
        for (BucketType j = 0; j < nodes_map[i]; ++j) {
          start += sums[j];
        }

        tree_nodes[nodes_map[i]] =
            BuildRecursive(Out.cut(start, start + sums[nodes_map[i]]),
                           In.cut(start, start + sums[nodes_map[i]]), dim,
                           box_seq[nodes_map[i]]);
      },
      1);

  return BT::template BuildInnerTree<Leaf, Interior>(1, pivots, tree_nodes);
}

}  // namespace pmos

#endif  // PMOS_B_TREE_IMPL_BATCH_OP_B_BUILD_TREE_HPP_

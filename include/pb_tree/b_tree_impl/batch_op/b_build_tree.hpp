#ifndef PBTREE_B_TREE_IMPL_BATCH_OP_B_BUILD_TREE_HPP_
#define PBTREE_B_TREE_IMPL_BATCH_OP_B_BUILD_TREE_HPP_

#include <parlay/range.h>
#include <parlay/slice.h>
#include <parlay/type_traits.h>

#include <cstddef>

#include "../../b_tree.h"
#include "parlay/delayed_sequence.h"
#include "parlay/parallel.h"
#include "parlay/primitives.h"

namespace pb_tree {
template<typename TT>
template<typename Range>
void
BTree<TT>::Build(Range&& In)
{
  static_assert(parlay::is_random_access_range_v<Range>);
  static_assert(
    parlay::is_less_than_comparable_v<parlay::range_reference_type_t<Range>>);
  static_assert(std::is_constructible_v<parlay::range_value_type_t<Range>,
                                        parlay::range_reference_type_t<Range>>);

  auto A = parlay::make_slice(std::forward<Range>(In));
  Build_(A);
}

template<typename TT>
template<typename Range>
void
BTree<TT>::Build_(Range In)
{
  auto sort_seq = parlay::sort(In);
  this->root_ = BuildRecursive(parlay::make_slice(sort_seq));
  assert(this->root_ != nullptr);
}

template<typename TT>
TT::Node*
BTree<TT>::AssembleInteriorLevels(TT::NodeSlice nodes, TT::SplitSlice keys)
{
  auto aux_node_seq = TT::NodeSeq::uninitialized(nodes.size());
  auto aux_key_seq = TT::KeySeq::uninitialized(keys.size());
  auto node_in = nodes;
  auto node_out = parlay::make_slice(aux_node_seq);
  auto key_in = keys;
  auto key_out = parlay::make_slice(aux_key_seq);

  auto swap_key_node = [&](SizeT bkt_n) {
    key_in = key_in.cut(0, bkt_n - 1);
    key_out = key_out.cut(0, bkt_n - 1);
    node_in = node_in.cut(0, bkt_n);
    node_out = node_out.cut(0, bkt_n);
    std::ranges::swap(key_in, key_out);
    std::ranges::swap(node_in, node_out);
  };

  while (node_in.size() >= TT::kBufUB) {
    SizeT interior_bkt_n = GenerateInteriorNodes(
      node_in,
      key_in,
      GenNodesOutputHandler{
        .set_key = [&](SizeT i, auto key) { key_out[i] = key; },
        .set_node = [&](SizeT i, auto node) { node_out[i] = node; } });
    swap_key_node(interior_bkt_n);
    assert(parlay::is_sorted(key_in, [&](TT::K a, TT::K b) { return a < b; }));
  }

  return AllocInteriorNode<Interior, TT>(node_in, key_in);
}

template<typename TT>
template<typename Range>
TT::Node*
BTree<TT>::BuildRecursive(Range In)
{
  if (In.size() == 0) {
    return AllocEmptyLeafNode<Range, Leaf>();
  }

  if (In.size() < TT::kBufUB) {
    return AllocNormalLeafNode<Range, Leaf>(In);
  }

  SizeT estimated_bkt_n = (In.size() / TT::kBufMD) + 1;
  auto node_seq = parlay::sequence<Node*>::uninitialized(estimated_bkt_n);
  auto key_seq =
    parlay::sequence<typename TT::K>::uninitialized(estimated_bkt_n);
  auto node_out = parlay::make_slice(node_seq);
  auto key_out = parlay::make_slice(key_seq);

  SizeT bkt_n = GenerateLeafNodes(
    In,
    GenNodesOutputHandler{
      .set_key = [&](SizeT i, auto key) { key_out[i] = key; },
      .set_node = [&](SizeT i, auto node) { node_out[i] = node; } });
  assert(parlay::is_sorted(key_out.cut(0, bkt_n - 1),
                           [&](TT::K a, TT::K b) { return a < b; }));

  this->root_ =
    AssembleInteriorLevels(node_out.cut(0, bkt_n), key_out.cut(0, bkt_n - 1));
  return this->root_;
}

} // namespace pb_tree

#endif // PBTREE_B_TREE_IMPL_BATCH_OP_B_BUILD_TREE_HPP_

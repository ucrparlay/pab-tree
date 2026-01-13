#ifndef PMOS_B_TREE_IMPL_BATCH_OP_B_BUILD_TREE_HPP_
#define PMOS_B_TREE_IMPL_BATCH_OP_B_BUILD_TREE_HPP_

#include <parlay/range.h>
#include <parlay/slice.h>
#include <parlay/type_traits.h>

#include <cstddef>

#include "../../b_tree.h"
#include "parlay/delayed_sequence.h"
#include "parlay/parallel.h"
#include "parlay/primitives.h"

namespace pmos {
template <typename TT>
template <typename Range>
void BTree<TT>::Build(Range&& In) {
  static_assert(parlay::is_random_access_range_v<Range>);
  static_assert(
      parlay::is_less_than_comparable_v<parlay::range_reference_type_t<Range>>);
  static_assert(std::is_constructible_v<parlay::range_value_type_t<Range>,
                                        parlay::range_reference_type_t<Range>>);

  EntrySlice A = parlay::make_slice(In);
  Build_(A);
}

template <typename TT>
void BTree<TT>::Build_(EntrySlice In) {
  auto sort_seq = parlay::sort(In);  // TODO: more optimization within sort
  this->root_ = BuildRecursive(parlay::make_slice(sort_seq));
  assert(this->root_ != nullptr);
  return;
}

template <typename TT>
TT::Node* BTree<TT>::BuildRecursive(EntrySlice In) {
  if (In.size() == 0) {
    return AllocEmptyLeafNode<EntrySlice, Leaf>();
  }

  if (In.size() < TT::kBufUB) {
    return AllocNormalLeafNode<EntrySlice, Leaf>(In);
  }

  auto tree_node_seq_in =
      parlay::sequence<Node*>::uninitialized(In.size() / TT::kBufMD + 1);
  auto tree_node_seq_out =
      parlay::sequence<Node*>::uninitialized(In.size() / TT::kBufMD + 1);
  auto node_in = parlay::make_slice(tree_node_seq_in);
  auto node_out = parlay::make_slice(tree_node_seq_out);

  auto key_seq_in = parlay::sequence<typename TT::K>::uninitialized(
      In.size() / TT::kBufMD + 1);
  auto key_seq_out = parlay::sequence<typename TT::K>::uninitialized(
      In.size() / TT::kBufMD + 1);
  auto key_in = parlay::make_slice(key_seq_in);
  auto key_out = parlay::make_slice(key_seq_out);

  auto swap_key_node = [&](size_t bkt_n) {
    key_in = key_in.cut(0, bkt_n - 1);
    key_out = key_out.cut(0, bkt_n - 1);
    node_in = node_in.cut(0, bkt_n);
    node_out = node_out.cut(0, bkt_n);
    std::ranges::swap(key_in, key_out);
    std::ranges::swap(node_in, node_out);
  };

  // NOTE: build leaf
  SizeT bkt_n = GenerateLeafNodes(
      In, GenNodesOutputHandler{
              .set_key = [&](SizeT i, auto key) { key_out[i] = key; },
              .set_node = [&](SizeT i, auto node) { node_out[i] = node; }});
  swap_key_node(bkt_n);
  assert(parlay::is_sorted(key_in, [&](TT::K a, TT::K b) { return a < b; }));

  // NOTE: build Interior
  while (node_in.size() >= TT::kBufUB) {
    SizeT interior_bkt_n = GenerateInteriorNodes(
        node_in, key_in,
        GenNodesOutputHandler{
            .set_key = [&](SizeT i, auto key) { key_out[i] = key; },
            .set_node = [&](SizeT i, auto node) { node_out[i] = node; }});
    swap_key_node(interior_bkt_n);
    assert(parlay::is_sorted(key_in, [&](TT::K a, TT::K b) { return a < b; }));
  }

  this->root_ = AllocInteriorNode<Interior, TT>(node_in, key_in);
  return this->root_;
}

}  // namespace pmos

#endif  // PMOS_B_TREE_IMPL_BATCH_OP_B_BUILD_TREE_HPP_

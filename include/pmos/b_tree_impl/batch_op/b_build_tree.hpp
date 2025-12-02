#ifndef PMOS_B_TREE_IMPL_BATCH_OP_B_BUILD_TREE_HPP_
#define PMOS_B_TREE_IMPL_BATCH_OP_B_BUILD_TREE_HPP_

#include <parlay/range.h>
#include <parlay/slice.h>
#include <parlay/type_traits.h>

#include <cstddef>

#include "parlay/delayed_sequence.h"
#include "parlay/parallel.h"
#include "parlay/primitives.h"
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

  EntrySlice A = parlay::make_slice(In);
  Build_(A);
}

template <typename TT>
void BTree<TT>::Build_(EntrySlice In) {
  // puts("-----");
  auto sort_seq = parlay::sort(In);  // TODO: more optimization within sort
  // sort_seq = parlay::remove_duplicates_ordered(sort_seq);
  // for (int i = 0; i < 10; i++) {
  //   std::cout << TT::GetKey(sort_seq[i]) << " ";
  // }
  // std::cout << sort_seq.size() << std::endl;
  this->root_ = BuildRecursive(parlay::make_slice(sort_seq));
  assert(this->root_ != nullptr);
  return;
}

template <typename TT>
Node* BTree<TT>::BuildRecursive(EntrySlice In) {
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
  // size_t item_n = In.size();
  // size_t bkt_n = item_n / TT::kBufMD;
  // assert(bkt_n > 0);
  // parlay::parallel_for(0, bkt_n, [&](size_t i) {
  //   if (i < bkt_n - 1) {
  //     // key_out[i] = TT::GetKey(In[idx_to_pack[i + 1]]);
  //     key_out[i] = Entry::GetKey(In[GetBktEnd(i, bkt_n, item_n)]);
  //   }
  //   size_t ele_l = GetBktSt(i, bkt_n, item_n);
  //   size_t ele_r = (i == bkt_n - 1) ? item_n : GetBktEnd(i, bkt_n, item_n);
  //   node_out[i] = AllocNormalLeafNode<EntrySlice, Leaf>(In.cut(ele_l,
  //   ele_r));
  // });
  size_t bkt_n = GenerateLeafNodes(In, key_out, node_out);
  swap_key_node(bkt_n);
  assert(parlay::is_sorted(key_in, [&](TT::K a, TT::K b) { return a < b; }));

  // NOTE: build Interior
  while (node_in.size() >= TT::kBufUB) {
    // assert(key_in.size() == node_in.size() - 1);
    // item_n = node_in.size();
    // bkt_n = item_n / TT::kBufMD;
    // assert(bkt_n > 0);
    //
    // parlay::parallel_for(0, bkt_n, [&](size_t i) {
    //   if (i < bkt_n - 1) {
    //     key_out[i] = key_in[GetBktEnd(i, bkt_n, item_n) - 1];
    //   }
    //
    //   size_t node_l = GetBktSt(i, bkt_n, item_n);
    //   size_t node_r = (i == bkt_n - 1) ? item_n : GetBktEnd(i, bkt_n,
    //   item_n); size_t key_l = GetBktSt(i, bkt_n, item_n); size_t key_r =
    //       (i == bkt_n - 1) ? key_in.size() : GetBktEnd(i, bkt_n, item_n) - 1;
    //   node_out[i] = AllocInteriorNode<Interior, TT>(node_in.cut(node_l,
    //   node_r),
    //                                                 key_in.cut(key_l,
    //                                                 key_r));
    // });
    size_t interior_bkt_n =
        GenerateInteriorNodes(node_in, node_out, key_in, key_out);
    swap_key_node(interior_bkt_n);
    assert(parlay::is_sorted(key_in, [&](TT::K a, TT::K b) { return a < b; }));
  }

  this->root_ = AllocInteriorNode<Interior, TT>(node_in, key_in);
  return this->root_;
}

}  // namespace pmos

#endif  // PMOS_B_TREE_IMPL_BATCH_OP_B_BUILD_TREE_HPP_

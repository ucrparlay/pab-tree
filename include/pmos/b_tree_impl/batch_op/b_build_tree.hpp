#ifndef PMOS_B_TREE_IMPL_BATCH_OP_B_BUILD_TREE_HPP_
#define PMOS_B_TREE_IMPL_BATCH_OP_B_BUILD_TREE_HPP_

#include <parlay/range.h>
#include <parlay/slice.h>
#include <parlay/type_traits.h>

#include <cstddef>

#include "parlay/delayed_sequence.h"
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

  Slice A = parlay::make_slice(In);
  Build_(A);
}

template <typename TT>
void BTree<TT>::Build_(Slice In) {
  // this->root_ = BuildRecursive(A);
  auto sort_seq = parlay::sort(In);  // TODO: more optimization within sort
  sort_seq = parlay::remove_duplicates_ordered(sort_seq);
  std::cout << sort_seq.size() << std::endl;
  this->root_ = BuildRecursive(parlay::make_slice(sort_seq));
  assert(this->root_ != nullptr);
  return;
}

template <typename TT>
Node* BTree<TT>::BuildRecursive(Slice In) {
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

  auto build_level = [&](size_t input_size, auto build_node_func) {
    size_t bucket_num = input_size / TT::kBufMD;
    auto items_per_bucket =
        parlay::delayed_seq<typename TT::BallsType>(bucket_num, [&](size_t i) {
          return i < (input_size % TT::kBufMD) ? TT ::kBufMD + 1 : TT::kBufMD;
        });
    auto [idx_to_pack, node_sum] =
        parlay::scan(items_per_bucket, parlay::plus<typename TT::BallsType>());
    assert(node_sum == input_size);
    assert(idx_to_pack.size() == bucket_num);

    parlay::parallel_for(0, bucket_num,
                         [&](size_t i) { build_node_func(i, idx_to_pack); });
    for (int i = 0; i < bucket_num - 1 - 1; i++) {
      assert(key_out[i] <= key_out[i + 1]);
    }

    // swap the input slice
    key_in = key_in.cut(0, bucket_num - 1);
    key_out = key_out.cut(0, bucket_num - 1);
    node_in = node_in.cut(0, bucket_num);
    node_out = node_out.cut(0, bucket_num);
    std::ranges::swap(key_in, key_out);
    std::ranges::swap(node_in, node_out);

    assert(parlay::is_sorted(key_in, [&](TT::K a, TT::K b) { return a < b; }));
  };

  // NOTE: build leaf
  size_t item_num = In.size();
  build_level(item_num, [&](size_t i, auto const& idx_to_pack) {
    size_t bucket_num = idx_to_pack.size();
    if (i < bucket_num - 1) {
      key_out[i] = TT::GetKey(In[idx_to_pack[i + 1]]);
    }
    size_t ele_l = idx_to_pack[i];
    size_t ele_r = (i == bucket_num - 1) ? item_num : idx_to_pack[i + 1];
    node_out[i] = AllocNormalLeafNode<Slice, Leaf>(In.cut(ele_l, ele_r));
  });

  // NOTE: build Interior
  while (node_in.size() >= TT::kBufUB) {
    assert(key_in.size() == node_in.size() - 1);
    size_t node_num = node_in.size();
    size_t key_num = key_in.size();

    build_level(node_num, [&](size_t i, auto const& idx_to_pack) {
      size_t bucket_num = idx_to_pack.size();
      if (i < bucket_num - 1) {
        key_out[i] = key_in[idx_to_pack[i + 1] - 1];
      }
      size_t node_l = idx_to_pack[i];
      size_t node_r = (i == bucket_num - 1) ? node_num : idx_to_pack[i + 1];
      size_t key_l = idx_to_pack[i];
      size_t key_r = (i == bucket_num - 1) ? key_num : idx_to_pack[i + 1] - 1;
      node_out[i] = AllocInteriorNode<Interior, TT>(node_in.cut(node_l, node_r),
                                                    key_in.cut(key_l, key_r));
    });
  }

  this->root_ = AllocInteriorNode<Interior, TT>(node_in, key_in);
  return this->root_;
}

}  // namespace pmos

#endif  // PMOS_B_TREE_IMPL_BATCH_OP_B_BUILD_TREE_HPP_

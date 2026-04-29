#ifndef PBTREE_B_TREE_IMPL_VALIDATION_HPP_
#define PBTREE_B_TREE_IMPL_VALIDATION_HPP_

#include <parlay/parallel.h>

#include <concepts>
#include <cwchar>

#include "b_tree.h"
#include "parlay/primitives.h"

namespace pb_tree {
template<typename TT>
template<bool kCheckSize>
TT::Box
BTree<TT>::VerifyTreeProperty(Node const* T)
{
  if (T->is_leaf) {
    Leaf const* TL = static_cast<Leaf const*>(T);
    if constexpr (kCheckSize) {
      assert(T == this->root_ || (TL->GetEle().size() >= TT::kBufLB &&
                                  TL->GetEle().size() <= TT::kBufUB));
    }
    assert(!TL->is_sorted ||
           parlay::is_sorted(TL->GetEle(), [&](auto const& e1, auto const& e2) {
             return Entry::GetKey(e1) < Entry::GetKey(e2);
           }));
    return pb_tree::GetBox<TT>(TL->GetEle());
  }

  auto const* TI = static_cast<Interior const*>(T);

  // ensure the split number is 1 less than the child number
  assert(T == this->root_ ||
         (TI->GetNodeN() >= TT::kBufLB && TI->GetNodeN() <= TT::kBufUB));

  // ensure the split is increasing
  assert(parlay::is_sorted(TI->GetSplitSlice(),
                           [&](TT::K a, TT::K b) { return a < b; }));

  // check the box of each child node falls within the split between
  parlay::sequence<typename TT::Box> box_seq(TI->GetNodeN());
  parlay::parallel_for(0, TI->GetNodeN(), [&](size_t i) {
    box_seq[i] = VerifyTreeProperty<kCheckSize>(TI->tree_nodes[i]);
    auto node = TI->tree_nodes[i];
    auto box = box_seq[i];
    // assert(node->is_leaf || static_cast<Interior*>(node)->box == box);

    if (i == 0) {
      assert(box.second < TI->split[0]);
    } else if (i == TI->GetNodeN() - 1) {
      assert(box.first >= TI->split[i - 1]);
    } else {
      if (box.first < TI->split[i - 1] || box.second >= TI->split[i]) {
        std::cout << "Validation failed at interior node split check."
                  << std::endl;
        std::cout << "Box: [" << box.first << ", " << box.second
                  << "], Splits: [" << TI->split[i - 1] << ", " << TI->split[i]
                  << "]" << std::endl;
        assert(false);
      }
      assert(box.first >= TI->split[i - 1] && box.second < TI->split[i]);
    }
  });

  return pb_tree::GetBox<TT>(box_seq);
}

template<typename TT>
bool
BTree<TT>::VerifyUnoverlappedTree(Node const* T, TT::NodeSlice node_slice)
{
  int cnt = 0;
  for (int i = 0; i < node_slice.size(); i++) {
    auto node = node_slice[i];
    cnt += CheckContainNode(T, node) ? 1 : 0;
  }
  assert(cnt == 1);
  return cnt == 1;
}

// check whether node P is in the tree T
template<typename TT>
bool
BTree<TT>::CheckContainNode(Node const* T, Node const* P)
{
  if (T == P) {
    return true;
  }

  if (T->is_leaf) {
    return false;
  }

  auto TI = static_cast<Interior const*>(T);
  BooleanBufUBArr contain_arr;

  parlay::parallel_for(0, TI->GetNodeN(), [&](BufSizeT i) {
    contain_arr[i] = CheckContainNode(TI->tree_nodes[i], P);
  });

  return parlay::reduce(
    parlay::make_slice(contain_arr.begin(),
                       contain_arr.begin() + TI->GetNodeN()),
    parlay::binary_op([&](bool a, bool b) { return a | b; }, bool()));
}

template<typename TT>
TT::HeightT
BTree<TT>::CheckHeight(Node const* T)
{
  if (T->is_leaf) {
    return 1;
  }

  auto TI = static_cast<Interior const*>(T);
  parlay::sequence<typename TT::HeightT> heights(TI->GetNodeN());

  parlay::parallel_for(0, TI->GetNodeN(), [&](size_t i) {
    heights[i] = CheckHeight(TI->tree_nodes[i]);
  });

  assert(parlay::all_of(heights, [&](auto h) { return h == heights[0]; }));

  return heights[0] + 1;
}

template<typename TT>
template<typename Leaf, typename Interior>
int
BTree<TT>::GetHeight(Node const* T)
{
  if (T->is_leaf) {
    return 0;
  }

  Interior const* TI = static_cast<Interior const*>(T);
  parlay::sequence<int> heights(TI->GetNodeN());

  for (int i = 0; i < TI->GetNodeN(); i++) {
    heights[i] = GetHeight<Leaf, Interior>(TI->tree_nodes[i]);
  }

  return *parlay::max_element(heights) + 1;
}

template<typename TT>
template<typename Leaf, typename Interior>
int
BTree<TT>::GetTreeNodesNum(Node const* T)
{
  if (T->is_leaf) {
    return 1;
  }

  auto const* TI = static_cast<Interior const*>(T);

  int sum = 0;
  for (int i = 0; i < TI->GetNodeN(); i++) {
    sum += GetTreeNodesNum<Leaf, Interior>(TI->tree_nodes[i]);
  }

  return sum + 1;
}

template<typename TT>
template<bool kCheckSize, bool kVerbose>
bool
BTree<TT>::Validate(Node const* T)
{
  if (T == nullptr) {
    T = this->root_;
  }

  if constexpr (kVerbose) {
    std::cerr << std::endl;
    std::cerr << ">>> begin validate tree\n" << std::flush;
  }

  VerifyTreeProperty<kCheckSize>(T);

  // typename TT::HeightT height =
  //   this->root_->is_leaf ? 1 : static_cast<Interior*>(this->root_)->height;
  CheckHeight(T);
  // std::cerr << "Tree height validated.\n" << std::flush;

  if constexpr (kVerbose) {
    std::cerr << "<<< end validate tree\n" << std::flush;
  }
  return true;
}

template<typename TT>
std::tuple<bool, bool, typename TT::SizeT>
BTree<TT>::CheckTreeAugAfterDelete(Node* T)
{
  if (T->is_leaf) {
    return { GetNodeInvalidStatus(T),
             NodeContainInvalidSubtree(T),
             GetNodeReinsertSize(T) };
  }

  auto TI = static_cast<Interior*>(T);
  auto node_n = TI->GetNodeN();
  parlay::sequence<int> invalid_seq(node_n);
  parlay::sequence<typename TT::SizeT> invalid_subtree_num_seq(node_n);
  parlay::sequence<typename TT::SizeT> reinsert_size_seq(node_n);
  parlay::parallel_for(0, node_n, [&](size_t i) {
    auto [invalid, invalid_subtree_num, reinsert_size] =
      CheckTreeAugAfterDelete(TI->tree_nodes[i]);
    invalid_seq[i] = invalid ? 1 : 0;
    invalid_subtree_num_seq[i] = invalid_subtree_num;
    reinsert_size_seq[i] = reinsert_size;
  });

  auto sum_invalid = parlay::reduce(invalid_seq);
  auto sum_invalid_subtree_num = parlay::reduce(invalid_subtree_num_seq);
  auto sum_reinsert_size = parlay::reduce(reinsert_size_seq);

  if (T == this->root_) {
    assert(TI->valid == ((TI->GetNodeN() - sum_invalid) > 0));
  } else {
    assert(TI->valid == ((TI->GetNodeN() - sum_invalid) >= TT::kBufLB));
  }

  if (NodeIsValid(T)) {
    assert(sum_invalid_subtree_num == NodeContainInvalidSubtree(T));
    assert(sum_reinsert_size == GetNodeReinsertSize(T));
  } else {
    assert(NodeContainInvalidSubtree(T) == 1);
    assert(GetNodeReinsertSize(T) == TI->size);
  }

  return { GetNodeInvalidStatus(T),
           NodeIsValid(T) ? sum_invalid_subtree_num : 1,
           NodeIsValid(T) ? sum_reinsert_size : TI->size };
}

} // namespace pb_tree

#endif // PBTREE_B_TREE_IMPL_VALIDATION_HPP_

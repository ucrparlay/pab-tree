#ifndef PMOS_B_TREE_IMPL_VALIDATION_HPP_
#define PMOS_B_TREE_IMPL_VALIDATION_HPP_

#include <parlay/parallel.h>

#include <concepts>
#include <cwchar>

#include "b_tree.h"
#include "parlay/primitives.h"

namespace pmos {
template<typename TT>
template<typename Leaf, typename Interior>
TT::Box
BTree<TT>::VerifyTreeProperty(Node const* T)
{
  if (T->is_leaf) {
    Leaf const* TL = static_cast<Leaf const*>(T);
    assert(!TL->is_sorted ||
           parlay::is_sorted(TL->GetEle(), [&](auto const& e1, auto const& e2) {
             return Entry::GetKey(e1) < Entry::GetKey(e2);
           }));
    assert(T == this->root_ || (TL->GetEle().size() >= TT::kBufLB &&
                                TL->GetEle().size() <= TT::kBufUB));
    return pmos::GetBox<TT>(TL->GetEle());
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
    box_seq[i] = VerifyTreeProperty<Leaf, Interior>(TI->tree_nodes[i]);
    auto node = TI->tree_nodes[i];
    auto box = box_seq[i];
    assert(node->is_leaf || static_cast<Interior*>(node)->box == box);

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

  return pmos::GetBox<TT>(box_seq);
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
BTree<TT>::CheckHeight(Node const* T, TT::HeightT cur_height)
{
  if (T->is_leaf) {
    assert(cur_height == 1);
    return 1;
  }

  auto TI = static_cast<Interior const*>(T);
  assert(cur_height == TI->height);
  parlay::sequence<typename TT::HeightT> heights(TI->GetNodeN());

  parlay::parallel_for(0, TI->GetNodeN(), [&](size_t i) {
    heights[i] = CheckHeight(TI->tree_nodes[i], cur_height - 1);
  });

  assert(parlay::all_of(heights, [&](auto h) { return h == heights[0]; }));

  return TI->height;
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
template<typename Leaf, typename Interior>
void
BTree<TT>::Validate()
{
  std::cout << ">>> begin validate tree\n" << std::flush;

  VerifyTreeProperty<Leaf, Interior>(this->root_);
  std::cout << "Tree box validated.\n" << std::flush;

  typename TT::HeightT height =
    this->root_->is_leaf ? 1 : static_cast<Interior*>(this->root_)->height;
  CheckHeight(this->root_, height);
  std::cout << "Tree height validated.\n" << std::flush;

  std::cout << "<<< end validate tree\n" << std::flush;
  return;
}

// template <typename Point, typename DerivedTree, uint_fast8_t kSkHeight,
//           uint_fast8_t kImbaRatio>
// template <typename Leaf, typename Interior>
// size_t BTree<Point, DerivedTree, kSkHeight, kImbaRatio>::GetTreeHeight() {
//   size_t deep = 0;
//   return GetMaxTreeDepth<Leaf, Interior>(this->root_, deep);
// }

// template <typename Point, typename DerivedTree, uint_fast8_t kSkHeight,
//           uint_fast8_t kImbaRatio>
// template <typename Leaf, typename Interior>
// size_t BTree<Point, DerivedTree, kSkHeight, kImbaRatio>::GetMaxTreeDepth(
//     Node* T, size_t deep) {
//   if (T->is_leaf) {
//     return deep;
//   }

//   Interior* TI = static_cast<Interior*>(T);
//   if constexpr (IsBinaryNode<Interior>) {
//     int l = GetMaxTreeDepth<Leaf, Interior>(TI->left, deep + 1);
//     int r = GetMaxTreeDepth<Leaf, Interior>(TI->right, deep + 1);
//     return std::max(l, r);
//   } else if constexpr (IsMultiNode<Interior>) {
//     size_t max_depth = 0;
//     for (size_t i = 0; i < TI->GetNodeN(); i++) {
//       max_depth = std::max(max_depth, GetMaxTreeDepth<Leaf, Interior>(
//                                           TI->tree_nodes[i], deep + 1));
//     }
//     return max_depth;
//   } else {
//     return 0;
//   }
// }

// template <typename Point, typename DerivedTree, uint_fast8_t kSkHeight,
//           uint_fast8_t kImbaRatio>
// template <typename Leaf, typename Interior>
// double BTree<Point, DerivedTree, kSkHeight, kImbaRatio>::GetAveTreeHeight() {
//   // std::cout << IsBinaryNode<Interior> << std::endl;
//   parlay::sequence<size_t> heights(this->root_->size);
//   size_t idx = 0;
//   CountTreeHeights<Leaf, Interior>(this->root_, 0, idx, heights);
//   // auto kv = parlay::histogram_by_key( heights.cut( 0, idx ) );
//   // std::sort( kv.begin(), kv.end(),
//   //            [&]( auto a, auto b ) { return a.first < b.first; } );
//   // for ( auto i : kv )
//   //     std::cout << i.first << " " << i.second << std::endl;
//   return double(1.0 * parlay::reduce(heights.cut(0, idx)) / idx);
// }

// template <typename Point, typename DerivedTree, uint_fast8_t kSkHeight,
//           uint_fast8_t kImbaRatio>
// template <typename Leaf, typename Interior>
// size_t BTree<Point, DerivedTree, kSkHeight, kImbaRatio>::CountTreeNodesNum(
//     Node* T) {
//   if (T->is_leaf) {
//     return 1;
//   }

//   Interior* TI = static_cast<Interior*>(T);
//   if constexpr (IsBinaryNode<Interior>) {
//     size_t l, r;
//     parlay::par_do([&]() { l = CountTreeNodesNum<Leaf, Interior>(TI->left);
//     },
//                    [&]() { r = CountTreeNodesNum<Leaf, Interior>(TI->right);
//                    });
//     return l + r + 1;
//   } else {
//     size_t sum = 0;
//     for (int i = 0; i < TI->GetNodeN(); i++) {
//       sum += CountTreeNodesNum<Leaf, Interior>(TI->tree_nodes[i]);
//     }
//     return sum + 1;
//   }
// }

// template <typename Point, typename DerivedTree, uint_fast8_t kSkHeight,
//           uint_fast8_t kImbaRatio>
// template <typename Leaf, typename Interior>
// void BTree<Point, DerivedTree, kSkHeight, kImbaRatio>::CountTreeHeights(
//     Node* T, size_t deep, size_t& idx, parlay::sequence<size_t>& heights) {
//   if (T->is_leaf) {
//     heights[idx++] = deep;
//     return;
//   }

//   Interior* TI = static_cast<Interior*>(T);
//   if constexpr (IsBinaryNode<Interior>) {
//     CountTreeHeights<Leaf, Interior>(TI->left, deep + 1, idx, heights);
//     CountTreeHeights<Leaf, Interior>(TI->right, deep + 1, idx, heights);
//   } else if constexpr (IsMultiNode<Interior>) {
//     for (size_t i = 0; i < TI->tree_nodes.size(); i++) {
//       CountTreeHeights<Leaf, Interior>(TI->tree_nodes[i], deep + 1, idx,
//                                        heights);
//     }
//   } else {
//     return;
//   }
//   return;
// }

} // namespace pmos

#endif // PMOS_B_TREE_IMPL_VALIDATION_HPP_

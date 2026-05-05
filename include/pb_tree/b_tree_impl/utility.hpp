#ifndef PBTREE_B_TREE_IMPL_UTILITY_HPP_
#define PBTREE_B_TREE_IMPL_UTILITY_HPP_

#include <algorithm>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <numeric>
#include <tuple>
#include <type_traits>
#include <utility>

#include "pb_tree/b_tree.h"

namespace pb_tree {
template<typename TT>
template<typename Range>
void
BTree<TT>::GetEntries(Range& Out)
{
  this->FlattenRec<typename TT::EntrySlice, false, false>(
    this->root_,
    parlay::make_slice(Out.begin(), Out.begin() + this->root_->size));
}

} // namespace pb_tree

#endif // PBTREE_B_TREE_IMPL_UTILITY_HPP_

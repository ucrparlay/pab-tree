#ifndef PABTREE_B_TREE_IMPL_UTILITY_HPP_
#define PABTREE_B_TREE_IMPL_UTILITY_HPP_

#include <algorithm>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <numeric>
#include <tuple>
#include <type_traits>
#include <utility>

#include "pab_tree/b_tree.h"

namespace pab_tree
{

template <typename TT>
template <typename Range>
void btree<TT>::get_entries(Range &Out) const
{
	if (this->root_ == nullptr) [[unlikely]] {
		return;
	}
	this->flatten_rec<typename TT::entry_slice, false, false>(
		this->root_,
		parlay::make_slice(Out.begin(), Out.begin() + this->root_->size));
}

} // namespace pab_tree

#endif // PABTREE_B_TREE_IMPL_UTILITY_HPP_

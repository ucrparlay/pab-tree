#ifndef PABTREE_B_TREE_IMPL_UTILITY_HPP_
#define PABTREE_B_TREE_IMPL_UTILITY_HPP_

#include <algorithm>
#include <cassert>
#include <compare>
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
	// Checked here, not deeper: Out.begin() + size is already out of
	// bounds by the time flatten_rec sees it.
	assert(Out.size() >= this->root_->size);
	this->flatten_rec<typename TT::entry_slice, false, false>(
		this->root_,
		parlay::make_slice(Out.begin(), Out.begin() + this->root_->size));
}

template <typename TT> TT::entry_seq btree<TT>::to_seq() const
{
	if (this->root_ == nullptr) [[unlikely]] {
		return typename TT::entry_seq();
	}
	// Leaves are ordered against each other, so sorting each leaf is
	// enough to put the whole flatten in key order.
	typename TT::entry_seq out =
		parlay::sequence<E>::uninitialized(this->root_->size);
	this->flatten_rec<typename TT::entry_slice, false, true>(
		this->root_, parlay::make_slice(out.begin(), out.end()));
	return out;
}

template <typename TT> bool btree<TT>::operator==(btree const &other) const
{
	if (this->size() != other.size()) {
		return false;
	}
	if (this == &other) {
		return true;
	}

	auto const a = this->to_seq();
	auto const b = other.to_seq();
	return std::ranges::equal(a, b, [](E const &x, E const &y) {
		return TT::key_equal(Entry::get_key(x), Entry::get_key(y)) &&
			   Entry::get_val(x) == Entry::get_val(y);
	});
}

template <typename TT>
std::weak_ordering btree<TT>::operator<=>(btree const &other) const
{
	if (this == &other) {
		return std::weak_ordering::equivalent;
	}

	auto const a = this->to_seq();
	auto const b = other.to_seq();
	auto const n = std::min(a.size(), b.size());
	for (size_t i = 0; i < n; ++i) {
		auto const &ka = Entry::get_key(a[i]);
		auto const &kb = Entry::get_key(b[i]);
		if (ka < kb) {
			return std::weak_ordering::less;
		}
		if (kb < ka) {
			return std::weak_ordering::greater;
		}

		auto const &va = Entry::get_val(a[i]);
		auto const &vb = Entry::get_val(b[i]);
		if (va < vb) {
			return std::weak_ordering::less;
		}
		if (vb < va) {
			return std::weak_ordering::greater;
		}
	}
	return a.size() <=> b.size();
}

} // namespace pab_tree

#endif // PABTREE_B_TREE_IMPL_UTILITY_HPP_

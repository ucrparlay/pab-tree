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

template <typename type_trait>
template <typename Range>
void btree<type_trait>::get_entries(Range &Out) const
{
	if (this->root_ == nullptr) [[unlikely]] {
		return;
	}
	// Checked here, not deeper: Out.begin() + size is already out of
	// bounds by the time flatten_rec sees it.
	assert(Out.size() >= this->root_->size);
	this->flatten_rec<typename type_trait::entry_slice, false, false>(
		this->root_,
		parlay::make_slice(Out.begin(), Out.begin() + this->root_->size));
}

template <typename type_trait>
type_trait::entry_seq btree<type_trait>::to_seq() const
{
	if (this->root_ == nullptr) [[unlikely]] {
		return typename type_trait::entry_seq();
	}
	// Leaves are ordered against each other, so sorting each leaf is
	// enough to put the whole flatten in key order.
	typename type_trait::entry_seq out =
		parlay::sequence<entry_type>::uninitialized(this->root_->size);
	this->flatten_rec<typename type_trait::entry_slice, false, true>(
		this->root_, parlay::make_slice(out.begin(), out.end()));
	return out;
}

template <typename type_trait>
bool btree<type_trait>::operator==(btree const &other) const
{
	if (this->size() != other.size()) {
		return false;
	}
	if (this == &other) {
		return true;
	}

	auto const a = this->to_seq();
	auto const b = other.to_seq();
	return std::ranges::equal(
		a, b, [](entry_type const &x, entry_type const &y) {
			return type_trait::key_equal(entry_traits::get_key(x),
										 entry_traits::get_key(y)) &&
				   entry_traits::get_val(x) == entry_traits::get_val(y);
		});
}

template <typename type_trait>
std::weak_ordering btree<type_trait>::operator<=>(btree const &other) const
{
	if (this == &other) {
		return std::weak_ordering::equivalent;
	}

	auto const a = this->to_seq();
	auto const b = other.to_seq();
	auto const n = std::min(a.size(), b.size());
	for (size_t i = 0; i < n; ++i) {
		auto const &ka = entry_traits::get_key(a[i]);
		auto const &kb = entry_traits::get_key(b[i]);
		if (ka < kb) {
			return std::weak_ordering::less;
		}
		if (kb < ka) {
			return std::weak_ordering::greater;
		}

		auto const &va = entry_traits::get_val(a[i]);
		auto const &vb = entry_traits::get_val(b[i]);
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

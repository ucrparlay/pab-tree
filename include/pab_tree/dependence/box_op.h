#ifndef PABTREE_DEPENDENCE_BOX_OP_HPP_
#define PABTREE_DEPENDENCE_BOX_OP_HPP_

#include "pab_tree/dependence/type_trait.h"
#include <utility>

namespace pab_tree
{
// Least and greatest key under the tree's comparator. Derived from the
// numeric endpoints, so any comparator that keeps one of them extreme --
// std::less, std::greater, any monotone order -- works.
template <typename type_trait>
constexpr typename type_trait::key_type key_least()
{
	return type_trait::key_less(type_trait::num_comparator::min_val(),
								type_trait::num_comparator::max_val()) ?
			   type_trait::num_comparator::min_val() :
			   type_trait::num_comparator::max_val();
}

template <typename type_trait>
constexpr typename type_trait::key_type key_greatest()
{
	return type_trait::key_less(type_trait::num_comparator::min_val(),
								type_trait::num_comparator::max_val()) ?
			   type_trait::num_comparator::max_val() :
			   type_trait::num_comparator::min_val();
}

// Identity for the box fold below: any real key widens it.
template <typename type_trait> type_trait::box_type get_empty_box()
{
	return typename type_trait::box_type(key_greatest<type_trait>(),
										 key_least<type_trait>());
}

template <typename type_trait>
type_trait::box_type get_box(typename type_trait::box_type const &a,
							 typename type_trait::box_type const &b)
{
	return typename type_trait::box_type(
		type_trait::key_less(a.first, b.first) ? a.first : b.first,
		type_trait::key_less(a.second, b.second) ? b.second : a.second);
}

// Sequential on purpose: every caller passes at most k_buf_ub entries.
template <typename type_trait>
type_trait::box_type get_box(typename type_trait::const_entry_slice ele_slice)
{
	if (ele_slice.size() == 0) {
		return get_empty_box<type_trait>();
	}
	auto box = std::make_pair(type_trait::entry_traits::get_key(ele_slice[0]),
							  type_trait::entry_traits::get_key(ele_slice[0]));
	for (typename type_trait::buf_size_type i = 1; i < ele_slice.size(); i++) {
		auto key = type_trait::entry_traits::get_key(ele_slice[i]);
		if (type_trait::key_less(key, box.first)) {
			box.first = key;
		}
		if (type_trait::key_less(box.second, key)) {
			box.second = key;
		}
	}
	return box;
}

template <typename type_trait>
type_trait::box_type get_box(typename type_trait::entry_seq const &ele_seq)
{
	return get_box<type_trait>(parlay::make_slice(ele_seq));
}

template <typename type_trait>
type_trait::box_type get_box(typename type_trait::box_seq const &box_seq)
{
	auto box = box_seq[0];
	for (size_t i = 1; i < box_seq.size(); i++) {
		if (type_trait::key_less(box_seq[i].first, box.first)) {
			box.first = box_seq[i].first;
		}
		if (type_trait::key_less(box.second, box_seq[i].second)) {
			box.second = box_seq[i].second;
		}
	}
	return box;
}
} // namespace pab_tree

#endif // PABTREE_DEPENDENCE_BOX_OP_HPP_

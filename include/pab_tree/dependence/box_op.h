#ifndef PABTREE_DEPENDENCE_BOX_OP_HPP_
#define PABTREE_DEPENDENCE_BOX_OP_HPP_

#include "pab_tree/dependence/type_trait.h"
#include <utility>

namespace pab_tree
{
/*
 * Least and greatest key under the tree's comparator. Derived from the
 * numeric endpoints, so any comparator that keeps one of them extreme --
 * std::less, std::greater, any monotone order -- works.
 */
template <typename TT>
constexpr typename TT::key_type key_least()
{
	return TT::key_less(TT::num_comparator::min_val(),
						TT::num_comparator::max_val()) ?
			   TT::num_comparator::min_val() :
			   TT::num_comparator::max_val();
}

template <typename TT>
constexpr typename TT::key_type key_greatest()
{
	return TT::key_less(TT::num_comparator::min_val(),
						TT::num_comparator::max_val()) ?
			   TT::num_comparator::max_val() :
			   TT::num_comparator::min_val();
}

/* Identity for the box fold below: any real key widens it. */
template <typename TT>
TT::box_type get_empty_box()
{
	return typename TT::box_type(key_greatest<TT>(), key_least<TT>());
}

template <typename TT>
TT::box_type get_box(typename TT::box_type const &a,
					 typename TT::box_type const &b)
{
	return typename TT::box_type(
		TT::key_less(a.first, b.first) ? a.first : b.first,
		TT::key_less(a.second, b.second) ? b.second : a.second);
}

/* Sequential on purpose: every caller passes at most k_buf_ub entries. */
template <typename TT>
TT::box_type get_box(typename TT::const_entry_slice ele_slice)
{
	if (ele_slice.size() == 0) {
		return get_empty_box<TT>();
	}
	auto box = std::make_pair(TT::entry_traits::get_key(ele_slice[0]),
							  TT::entry_traits::get_key(ele_slice[0]));
	for (typename TT::buf_size_type i = 1; i < ele_slice.size(); i++) {
		auto key = TT::entry_traits::get_key(ele_slice[i]);
		if (TT::key_less(key, box.first)) {
			box.first = key;
		}
		if (TT::key_less(box.second, key)) {
			box.second = key;
		}
	}
	return box;
}

template <typename TT>
TT::box_type get_box(typename TT::entry_seq const &ele_seq)
{
	return get_box<TT>(parlay::make_slice(ele_seq));
}

template <typename TT>
TT::box_type get_box(typename TT::box_seq const &box_seq)
{
	auto box = box_seq[0];
	for (size_t i = 1; i < box_seq.size(); i++) {
		if (TT::key_less(box_seq[i].first, box.first)) {
			box.first = box_seq[i].first;
		}
		if (TT::key_less(box.second, box_seq[i].second)) {
			box.second = box_seq[i].second;
		}
	}
	return box;
}
} /* namespace pab_tree */

#endif /* PABTREE_DEPENDENCE_BOX_OP_HPP_ */

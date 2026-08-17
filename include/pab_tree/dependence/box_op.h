#ifndef PABTREE_DEPENDENCE_BOX_OP_HPP_
#define PABTREE_DEPENDENCE_BOX_OP_HPP_

#include "pab_tree/dependence/type_trait.h"
#include <utility>

namespace pab_tree
{
template <typename TT> TT::Box get_empty_box()
{
	return typename TT::Box(TT::Num::max_val(), TT::Num::min_val());
}

template <typename TT>
TT::Box get_box(typename TT::Box const &a, typename TT::Box const &b)
{
	return typename TT::Box(TT::Num::min(a.first, b.first),
							TT::Num::max(a.second, b.second));
}

// TODO: this can be parallelized (needs to define the min/max of a box)
template <typename TT> TT::Box get_box(typename TT::const_entry_slice ele_slice)
{
	if (ele_slice.size() == 0) {
		return get_empty_box<TT>();
	}
	auto box = std::make_pair(TT::Entry::get_key(ele_slice[0]),
							  TT::Entry::get_key(ele_slice[0]));
	for (typename TT::buf_size_type i = 1; i < ele_slice.size(); i++) {
		auto key = TT::Entry::get_key(ele_slice[i]);
		box.first = TT::Num::min(box.first, key);
		box.second = TT::Num::max(box.second, key);
	}
	return box;
}

template <typename TT> TT::Box get_box(typename TT::entry_seq const &ele_seq)
{
	return get_box<TT>(parlay::make_slice(ele_seq));
}

template <typename TT> TT::Box get_box(typename TT::box_seq const &box_seq)
{
	auto box = box_seq[0];
	for (size_t i = 1; i < box_seq.size(); i++) {
		box.first = TT::Num::min(box.first, box_seq[i].first);
		box.second = TT::Num::max(box.second, box_seq[i].second);
	}
	return box;
}
} // namespace pab_tree

#endif // PABTREE_DEPENDENCE_BOX_OP_HPP_

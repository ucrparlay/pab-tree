#ifndef PABTREE_B_TREE_IMPL_QUERY_OP_RANGE_COUNT_HPP_
#define PABTREE_B_TREE_IMPL_QUERY_OP_RANGE_COUNT_HPP_

#include <cassert>

#include "pab_tree/dependence/binary_search.h"
#include "parlay/slice.h"
#include "pab_tree/b_tree.h"

namespace pab_tree
{

template <typename TT>
auto btree<TT>::range_count(TT::Box const query_box) const -> size_type
{
	size_type rt_size = 0;
	if (this->root_ == nullptr) [[unlikely]] {
		return 0;
	}
	range_count_rec(this->root_, query_box, rt_size);
	return rt_size;
}

template <typename TT>
void btree<TT>::range_count_leaf(Node const *T, TT::Box const &query_box,
				 size_type &offset) const
{
	auto const *TL = static_cast<Leaf const *>(T);
	if (TL->is_sorted) {
		const auto l = TL->binary_search_key(query_box.first);
		// Narrow: search for r only in [l, size) since r >= l.
		const auto *elems = TL->elements.data() + l;
		const buf_size_type rem = static_cast<buf_size_type>(TL->size) -
					  static_cast<buf_size_type>(l);
		size_t lo = 0, hi = static_cast<size_t>(rem);
		while (hi - lo > 8) {
			size_t const mid = lo + ((hi - lo) >> 1);
			bool const go =
				Entry::get_key(elems[mid]) < query_box.second;
			lo = go ? mid + 1 : lo;
			hi = go ? hi : mid;
		}
		size_t cnt = 0;
		for (size_t i = lo; i < hi; ++i) {
			cnt += (Entry::get_key(elems[i]) < query_box.second) ?
				       1 :
				       0;
		}
		const auto r = l + static_cast<decltype(l)>(lo + cnt);
		assert(l <= r && r <= TL->size);
		offset += r - l;
		return;
	}

	// Unsorted leaf: branchless count
	size_type cnt = 0;
	for (buf_size_type i = 0; i < static_cast<buf_size_type>(TL->size);
	     i++) {
		auto const k = Entry::get_key(TL->elements[i]);
		cnt += (k >= query_box.first && k < query_box.second) ? 1 : 0;
	}
	offset += cnt;
}

template <typename TT>
void btree<TT>::range_count_rec(Node const *T, TT::Box const &query_box,
				size_type &offset) const
{
	// Iterative descent for single-child levels (avoids O(height) call overhead).
	while (!T->is_leaf) {
		auto const *TI = static_cast<Interior const *>(T);

		// Left boundary child
		const buf_size_type l =
			TI->get_node_id_containing_key(query_box.first);

		// Right boundary: narrow search to [l, split_n) since r >= l.
		const buf_size_type split_n = TI->get_split_n();
		const Split *sp_l = TI->split.data() + l;
		const size_t rem =
			static_cast<size_t>(split_n) - static_cast<size_t>(l);
		size_t lo = 0, hi = rem;
		while (hi - lo > 8) {
			size_t const mid = lo + ((hi - lo) >> 1);
			bool const go = (sp_l[mid] < query_box.second);
			lo = go ? mid + 1 : lo;
			hi = go ? hi : mid;
		}
		size_t cnt = 0;
		for (size_t i = lo; i < hi; ++i) {
			cnt += (sp_l[i] < query_box.second) ? 1 : 0;
		}
		const buf_size_type r =
			l + static_cast<buf_size_type>(lo + cnt);

		assert(l < TI->get_node_n());
		assert(r < TI->get_node_n());
		assert(TI->validate_key_in_node(l, query_box.first));
		assert(TI->validate_key_in_node(r, query_box.second) ||
		       TI->split[r] == query_box.second);

		if (l == r) {
			T = TI->tree_nodes[l];
			continue;
		}

		range_count_rec(TI->tree_nodes[l], query_box, offset);

		// Middle children: just sum their sizes (no traversal needed).
		for (buf_size_type i = l + 1; i < r; i++) {
			offset += TI->tree_nodes[i]->size;
		}

		range_count_rec(TI->tree_nodes[r], query_box, offset);
		return;
	}

	range_count_leaf(T, query_box, offset);
}

} // namespace pab_tree

#endif // PABTREE_B_TREE_IMPL_QUERY_OP_RANGE_COUNT_HPP_

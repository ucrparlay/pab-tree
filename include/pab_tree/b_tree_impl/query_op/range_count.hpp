#ifndef PABTREE_B_TREE_IMPL_QUERY_OP_RANGE_COUNT_HPP_
#define PABTREE_B_TREE_IMPL_QUERY_OP_RANGE_COUNT_HPP_

#include <cassert>

#include "pab_tree/dependence/binary_search.h"
#include "parlay/slice.h"
#include "pab_tree/b_tree.h"

namespace pab_tree
{

template <typename TT>
auto btree<TT>::range_count(TT::box_type const query_box) const -> size_type
{
	size_type rt_size = 0;
	if (this->root_ == nullptr ||
		!TT::key_less(query_box.first, query_box.second)) [[unlikely]] {
		return 0;
	}
	range_count_rec(this->root_, query_box, rt_size);
	return rt_size;
}

template <typename TT>
void btree<TT>::range_count_leaf(node_type const *node,
								 TT::box_type const &query_box,
								 size_type &offset) const
{
	auto const *leaf = static_cast<leaf_type const *>(node);
	if (leaf->is_sorted) {
		const auto l = leaf->binary_search_key(query_box.first);
		/* Narrow: search for r only in [l, size) since r >= l. */
		const auto *elems = leaf->elements.data() + l;
		const buf_size_type rem = static_cast<buf_size_type>(leaf->size) -
								  static_cast<buf_size_type>(l);
		/*
		 * Inlined on purpose: routing this through
		 * detail::binary_search cost +60 instructions and 21 extra
		 * compares in this path. Measured, not assumed.
		 */
		size_t lo = 0, hi = static_cast<size_t>(rem);
		while (hi - lo > 8) {
			size_t const mid = lo + ((hi - lo) >> 1);
			bool const go = TT::key_less(entry_traits::get_key(elems[mid]),
										 query_box.second);
			lo = go ? mid + 1 : lo;
			hi = go ? hi : mid;
		}
		size_t cnt = 0;
		for (size_t i = lo; i < hi; ++i) {
			cnt += TT::key_less(entry_traits::get_key(elems[i]),
								query_box.second) ?
					   1 :
					   0;
		}
		const auto r = l + static_cast<decltype(l)>(lo + cnt);
		assert(l <= r && r <= leaf->size);
		offset += r - l;
		return;
	}

	/* Unsorted leaf: branchless count */
	size_type cnt = 0;
	for (buf_size_type i = 0; i < static_cast<buf_size_type>(leaf->size); i++) {
		auto const k = entry_traits::get_key(leaf->elements[i]);
		cnt += (k >= query_box.first && k < query_box.second) ? 1 : 0;
	}
	offset += cnt;
}

template <typename TT>
void btree<TT>::range_count_rec(node_type const *node,
								TT::box_type const &query_box,
								size_type &offset) const
{
	/*
	 * Iterative descent for single-child levels (avoids O(height) call
	 * overhead).
	 */
	while (!node->is_leaf) {
		auto const *interior = static_cast<interior_type const *>(node);

		/* Left boundary child */
		const buf_size_type l =
			interior->get_node_id_containing_key(query_box.first);

		/* Right boundary: narrow search to [l, split_n) since r >= l. */
		const buf_size_type split_n = interior->get_split_n();
		const split_type *sp_l = interior->split.data() + l;
		const size_t rem =
			static_cast<size_t>(split_n) - static_cast<size_t>(l);
		/*
		 * Inlined on purpose: routing this through
		 * detail::binary_search cost +60 instructions and 21 extra
		 * compares in this path. Measured, not assumed.
		 */
		size_t lo = 0, hi = rem;
		while (hi - lo > 8) {
			size_t const mid = lo + ((hi - lo) >> 1);
			bool const go = TT::key_less(sp_l[mid], query_box.second);
			lo = go ? mid + 1 : lo;
			hi = go ? hi : mid;
		}
		size_t cnt = 0;
		for (size_t i = lo; i < hi; ++i) {
			cnt += TT::key_less(sp_l[i], query_box.second) ? 1 : 0;
		}
		const buf_size_type r = l + static_cast<buf_size_type>(lo + cnt);

		assert(l < interior->get_node_n());
		assert(r < interior->get_node_n());
		assert(interior->validate_key_in_node(l, query_box.first));
		assert(interior->validate_key_in_node(r, query_box.second) ||
			   TT::key_equal(interior->split[r], query_box.second));

		if (l == r) {
			node = interior->tree_nodes[l];
			continue;
		}

		range_count_rec(interior->tree_nodes[l], query_box, offset);

		/* Middle children: just sum their sizes (no traversal needed). */
		for (buf_size_type i = l + 1; i < r; i++) {
			offset += interior->tree_nodes[i]->size;
		}

		range_count_rec(interior->tree_nodes[r], query_box, offset);
		return;
	}

	range_count_leaf(node, query_box, offset);
}

} /* namespace pab_tree */

#endif /* PABTREE_B_TREE_IMPL_QUERY_OP_RANGE_COUNT_HPP_ */

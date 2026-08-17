#ifndef PABTREE_B_TREE_IMPL_QUERY_OP_RANGE_REPORT_HPP_
#define PABTREE_B_TREE_IMPL_QUERY_OP_RANGE_REPORT_HPP_

#include <cassert>

#include <algorithm>

#include "pab_tree/dependence/binary_search.h"
#include "parlay/parallel.h"
#include "parlay/slice.h"
#include "pab_tree/b_tree.h"

namespace pab_tree
{

template <typename TT>
template <typename Range>
auto btree<TT>::range_report(TT::Box const query_box, Range &&out) const
	-> size_type
{
	if (this->root_ == nullptr || query_box.first >= query_box.second)
		[[unlikely]] {
		return 0;
	}

	size_type rt_size = 0;
	range_report_iter(this->root_, query_box, rt_size,
			  parlay::make_slice(out));
	return rt_size;
}

template <typename TT>
template <typename Range>
void btree<TT>::range_report_leaf(Node const *T, TT::Box const &query_box,
				  size_type &offset, Range out) const
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
		assert(offset + (r - l) <= out.size() &&
		       "out is too small; size it with range_count");
		std::copy_n(TL->elements.begin() + l, r - l,
			    out.begin() + offset);
		offset += r - l;
		return;
	}

	// Unsorted leaf: linear scan
	for (buf_size_type i = 0; i < static_cast<buf_size_type>(TL->size);
	     i++) {
		auto const k = Entry::get_key(TL->elements[i]);
		if (k >= query_box.first && k < query_box.second) {
			assert(offset < out.size() &&
			       "out is too small; size it with range_count");
			out[offset++] = TL->elements[i];
		}
	}
}

template <typename TT>
template <typename Range>
void btree<TT>::range_report_iter(Node const *T, TT::Box const &query_box,
				  size_type &offset, Range out) const
{
	// Iterative descent: avoids O(height) recursive-call overhead for narrow
	// queries that fall inside a single child at each interior level.
	while (!T->is_leaf) {
		auto const *TI = static_cast<Interior const *>(T);
		auto *const *nodes =
			TI->tree_nodes
				.data(); // Node* const* → nodes[i] is Node*

		const buf_size_type l =
			TI->get_node_id_containing_key(query_box.first);

		// Right boundary child: narrow search to [l, split_n) since r >= l,
		// saving roughly half the binary-search work in the common case.
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
			// Entire query falls in one child — keep descending without recursion.
			T = nodes[l];
			continue;
		}

		// Query spans children [l, r].

		// Left boundary: partially covered, recurse.
		range_report_iter(nodes[l], query_box, offset, out);

		// Middle children [l+1, r): fully covered by the query.
		// Pre-compute each child's output position, then flatten in parallel.
		if (r > l + 1) {
			const buf_size_type mid_n = r - l - 1;
			size_buf_ub_arr off;
			off[0] = offset;
			for (buf_size_type k = 0; k < mid_n; ++k) {
				off[k + 1] = off[k] + nodes[l + 1 + k]->size;
			}
			assert(off[mid_n] <= out.size() &&
			       "out is too small; size it with range_count");
			offset = off[mid_n];
			parlay::parallel_for(
				0, mid_n,
				[&](buf_size_type k) {
					flatten_rec<Range, true, false>(
						nodes[l + 1 + k],
						out.cut(off[k], off[k + 1]));
				},
				1);
		}

		// Right boundary: partially covered, recurse.
		range_report_iter(nodes[r], query_box, offset, out);
		return;
	}

	// T is a leaf.
	range_report_leaf(T, query_box, offset, out);
}

// Sequential full-subtree flatten.
template <typename TT>
template <typename Range>
void btree<TT>::range_report_flatten_rec(Node const *T, size_type &offset,
					 Range out) const
{
	if (T->is_leaf) {
		auto const *TL = static_cast<Leaf const *>(T);
		assert(offset + TL->size <= out.size() &&
		       "out is too small; size it with range_count");
		std::copy_n(TL->elements.begin(), TL->size,
			    out.begin() + offset);
		offset += TL->size;
		return;
	}

	auto const *TI = static_cast<Interior const *>(T);
	const buf_size_type node_n = TI->get_node_n();
	for (buf_size_type i = 0; i < node_n; i++) {
		range_report_flatten_rec(TI->tree_nodes[i], offset, out);
	}
}

template <typename TT>
template <typename Range>
void btree<TT>::range_report_rec(Node const *T, TT::Box const &query_box,
				 size_type &offset, Range out) const
{
	if (T->is_leaf) {
		range_report_leaf(T, query_box, offset, out);
		return;
	}

	auto const *TI = static_cast<Interior const *>(T);
	auto l = TI->get_node_id_containing_key(query_box.first);
	auto r = TI->get_node_id_containing_key_exclusive(query_box.second);

	assert(l < TI->get_node_n());
	assert(r < TI->get_node_n());
	assert(TI->validate_key_in_node(l, query_box.first));
	assert(TI->validate_key_in_node(r, query_box.second) ||
	       TI->split[r] == query_box.second);

	// NOTE: visit nodes [l, r]

	if (l == r) { // falls into one node
		range_report_rec(TI->tree_nodes[l], query_box, offset, out);
		return;
	}

	range_report_rec(TI->tree_nodes[l], query_box, offset, out);
	range_report_rec(TI->tree_nodes[r], query_box, offset, out);

	if (r > l + 1) {
		for (buf_size_type i = l + 1; i < r; i++) {
			flatten_rec<decltype(out), false>(
				TI->tree_nodes[i],
				out.cut(offset,
					offset + TI->tree_nodes[i]->size));
			offset += TI->tree_nodes[i]->size;
		}
	}
}
} // namespace pab_tree

#endif // PABTREE_B_TREE_IMPL_QUERY_OP_RANGE_REPORT_HPP_

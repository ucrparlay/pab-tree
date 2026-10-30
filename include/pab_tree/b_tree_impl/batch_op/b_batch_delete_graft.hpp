#ifndef PABTREE_B_TREE_IMPL_BATCH_OP_B_BATCH_DELETE_GRAFT_HPP_
#define PABTREE_B_TREE_IMPL_BATCH_OP_B_BATCH_DELETE_GRAFT_HPP_

#include <algorithm>
#include <array>
#include <cassert>
#include <optional>

#include "pab_tree/b_tree.h"
#include "parlay/parallel.h"
#include "parlay/primitives.h"
#include "parlay/slice.h"

/*
 * Batch delete below a valid root. batch_delete_recursive has balanced the
 * leaves and counted what every subtree hands over. One pass cuts the
 * largest valid subtrees out of every invalid subtree, with the entries of
 * underfull leaves, into one array already in key order; one descent puts
 * them back into the tree that is left. docs/plan_graft_delete_2026-09.md.
 */

namespace pab_tree
{
template <typename TT>
template <typename LeafSortPred>
void btree<TT>::graft_repair(const TT::pattern_type leaf_entries_pattern,
							 LeafSortPred const &leaf_sort_pred)
{
	assert(!this->root_->is_leaf && node_is_valid(this->root_) &&
		   node_contain_invalid_subtree(this->root_));
	const size_type n =
		static_cast<interior_type *>(this->root_)->reinsert_size;
	const height_type levels = graft_height(this->root_);

	/* 1. Cut out every invalid subtree, in key order; no sort. */
	auto keys = TT::key_seq::uninitialized(n);
	auto nodes = TT::node_seq::uninitialized(n);
	auto entries = TT::entry_seq::uninitialized(n);
	auto heights = parlay::sequence<height_type>::uninitialized(n);
	opt_key const edge = graft_detach_rec(
		this->root_, std::nullopt, levels,
		graft_pool{ parlay::make_slice(keys), parlay::make_slice(nodes),
					parlay::make_slice(entries), parlay::make_slice(heights) });
	if (n == 0) {
		return;
	}
	/* Strictly increasing: no key is at or below the one before it. */
	assert(parlay::is_sorted(keys, [](key_type const &a, key_type const &b) {
		return !TT::key_less(b, a);
	}));

	/* 2. Item positions grouped by height; loose entries in the last group. */
	auto const grouped = parlay::counting_sort(
		parlay::iota<size_type>(n), static_cast<size_t>(levels) + 1,
		[&](size_type i) -> size_type {
			return heights[i] == k_entry_source ? levels : heights[i];
		});
	auto const &hpos = grouped.first;
	auto const &hoff = grouped.second;
	graft_batch const in{ keys.begin(), nodes.begin(), entries.begin(),
						  hpos.begin(), hoff.begin(),  levels };

	auto scratch = node_split_pair_seq::uninitialized(2 * n);
	/* A root that split hands up its parts: new levels on top. */
	auto grow_root = [&](size_type k, node_split_pair_slice o) {
		if (k == 0) {
			return;
		}
		auto up_nodes =
			parlay::tabulate(k, [&](size_t i) { return o[i].first; });
		auto up_keys =
			parlay::tabulate(k - 1, [&](size_t i) { return o[i].second; });
		this->root_ = assemble_interior_levels(parlay::make_slice(up_nodes),
											   parlay::make_slice(up_keys));
	};

	/*
	 * 3. Inputs left of all of the tree's content (its first nodes went)
	 * have no left neighbour to join: prepend them along the leftmost
	 * path, tallest first, moving the edge after each height. Rare.
	 */
	size_type first = 0;
	if (edge) {
		first = static_cast<size_type>(
			std::lower_bound(keys.begin(), keys.end(), *edge,
							 [](key_type const &a, key_type const &b) {
								 return TT::key_less(a, b);
							 }) -
			keys.begin());
	}
	if (first > 0) {
		auto lead = [&](size_t h) {
			auto const *b = hpos.begin() + hoff[h];
			return parlay::make_slice(
				b, std::lower_bound(b, hpos.begin() + hoff[h + 1], first));
		};
		opt_key e = edge;
		for (height_type h = levels; h-- > 0;) {
			auto const pos = lead(h);
			if (pos.size() == 0) {
				continue;
			}
			auto items = parlay::tabulate(
				pos.size(), [&](size_t i) { return nodes[pos[i]]; });
			auto const bounds = parlay::tabulate(
				pos.size(), [&](size_t i) { return keys[pos[i]]; });
			auto const o = parlay::make_slice(scratch).cut(0, 2 * pos.size());
			grow_root(
				graft_rec(this->root_, graft_height(this->root_), h,
						  parlay::make_slice(items),
						  parlay::make_slice(bounds.cbegin(), bounds.cend()), o,
						  e),
				o);
			if (TT::key_less(bounds[0], *e)) {
				e = bounds[0];
			}
		}
		auto const pos = lead(levels);
		if (pos.size() > 0) {
			auto const lead_entries = parlay::tabulate(
				pos.size(), [&](size_t i) { return entries[pos[i]]; });
			auto const o = parlay::make_slice(scratch).cut(0, 2 * pos.size());
			grow_root(
				batch_insert_recursive(this->root_,
									   parlay::make_slice(lead_entries.cbegin(),
														  lead_entries.cend()),
									   o, leaf_entries_pattern, leaf_sort_pred),
				o);
		}
	}

	/* 4. Everything else in one descent. */
	if (first < n) {
		auto const o = parlay::make_slice(scratch).cut(2 * first, 2 * n);
		grow_root(graft_all_rec(this->root_, graft_height(this->root_), first,
								n, in, o, leaf_entries_pattern, leaf_sort_pred),
				  o);
	}

#ifdef DEBUG
	[[maybe_unused]] const bool tree_ok = this->validate<true, false>();
	assert(tree_ok);
#endif /* DEBUG */
}

/*
 * in[l, r) is inside node's range and sorted by key. Items one level below
 * node join its child list first (graft_join); with none, the inputs route
 * as in batch insert. node's size moves by what its touched children
 * gained, so untouched children are not read.
 */
template <typename TT>
template <typename LeafSortPred>
TT::size_type
btree<TT>::graft_all_rec(node_type *node, height_type height, size_type l,
						 size_type r, graft_batch const &in,
						 node_split_pair_slice out,
						 const TT::pattern_type leaf_entries_pattern,
						 LeafSortPred const &leaf_sort_pred)
{
	assert(!node->is_leaf && height >= 1 && l < r);
	assert(out.size() == 2 * (r - l));

	if (static_cast<size_type>(height - 1) < in.level_n) {
		size_type const *const b = in.hpos + in.hoff[height - 1];
		size_type const *const e = in.hpos + in.hoff[height];
		size_type const *const own = std::lower_bound(b, e, l);
		const size_type own_n =
			static_cast<size_type>(std::lower_bound(own, e, r) - own);
		if (own_n > 0) {
			return graft_join(node, height, l, r, own, own_n, in, out,
							  leaf_entries_pattern, leaf_sort_pred);
		}
	}

	auto interior = static_cast<interior_type *>(node);
	const buf_size_type m = interior->get_node_n();
	const size_type n = r - l;
	size_buf_ub_arr split_pos;
	partition_batch(split_pos, parlay::make_slice(in.keys + l, in.keys + r),
					interior);
	auto first_of = [&](buf_size_type i) -> size_type {
		return i == 0 ? 0 : split_pos[i - 1];
	};
	auto end_of = [&](buf_size_type i) -> size_type {
		return i + 1 == m ? n : split_pos[i];
	};

	size_buf_ub_arr ret;
	buf_size_buf_ub_arr touched;
	buf_size_type cnt = 0;
	for (buf_size_type i = 0; i < m; ++i) {
		ret[i] = 0;
		if (first_of(i) != end_of(i)) {
			touched[cnt++] = i;
		}
	}

	size_buf_ub_arr old;
	parlay::parallel_for(
		0, cnt,
		[&](buf_size_type k) {
			const buf_size_type i = touched[k];
			const size_type lo = first_of(i);
			const size_type hi = end_of(i);
			auto *child = interior->tree_nodes[i];
			old[k] = child->size;
			auto const o = out.cut(2 * lo, 2 * hi);
			if (height == 1) { /* only entries reach a leaf */
				ret[i] = batch_insert_recursive(
					child,
					parlay::make_slice(in.entries + l + lo,
									   in.entries + l + hi),
					o, leaf_entries_pattern, leaf_sort_pred);
			} else {
				ret[i] = graft_all_rec(child, height - 1, l + lo, l + hi, in, o,
									   leaf_entries_pattern, leaf_sort_pred);
			}
		},
		1);

	size_type new_node_n = m;
	size_type size = interior->size;
	for (buf_size_type k = 0; k < cnt; ++k) {
		const buf_size_type i = touched[k];
		size -= old[k];
		if (ret[i] == 0) {
			size += interior->tree_nodes[i]->size;
			continue;
		}
		new_node_n += ret[i] - 1;
		auto const *o = out.begin() + 2 * first_of(i);
		for (size_type t = 0; t < ret[i]; ++t) {
			size += o[t].first->size;
		}
	}
	if (new_node_n > TT::k_buf_ub) {
		return handling_interior_split_overflow(node, out, split_pos,
												new_node_n, ret);
	}
	if (new_node_n > m) { /* a child that split gives way to its parts */
		typename interior_type::node_arr fnode;
		typename interior_type::split_arr fsep;
		size_type w = 0;
		for (buf_size_type i = 0; i < m; ++i) {
			if (i > 0) {
				fsep[w - 1] = interior->split[i - 1];
			}
			if (ret[i] == 0) {
				fnode[w++] = interior->tree_nodes[i];
				continue;
			}
			auto const *o = out.begin() + 2 * first_of(i);
			for (size_type t = 0; t < ret[i]; ++t) {
				if (t > 0) {
					fsep[w - 1] = o[t - 1].second;
				}
				fnode[w++] = o[t].first;
			}
		}
		assert(w == new_node_n);
		std::copy_n(fnode.begin(), w, interior->tree_nodes.begin());
		std::copy_n(fsep.begin(), w - 1, interior->split.begin());
		interior->node_n = static_cast<buf_size_type>(w);
	}
	interior->size = size;
	interior->update_aug();
	return 0;
}

/*
 * node takes the items at own[0, own_n) as children, each right after the
 * child its bound routes to, with the bound as separator: a removed node's
 * key range went to its left neighbour. Every other input in [l, r) is
 * shorter and routes by the merged list, so it can land inside a joined
 * item. node is rebuilt once, after its children returned. Buffers are on
 * the stack; the heap only when node takes more than k_buf_ub children.
 */
template <typename TT>
template <typename LeafSortPred>
TT::size_type
btree<TT>::graft_join(node_type *node, height_type height, size_type l,
					  size_type r, size_type const *own, size_type own_n,
					  graft_batch const &in, node_split_pair_slice out,
					  const TT::pattern_type leaf_entries_pattern,
					  LeafSortPred const &leaf_sort_pred)
{
	auto interior = static_cast<interior_type *>(node);
	const buf_size_type m = interior->get_node_n();
	const size_type n = r - l;
	const size_type merged_n = m + own_n;

	const bool big = merged_n > TT::k_buf_ub;
	typename interior_type::node_arr mnode_s;
	typename interior_type::split_arr msep_s;
	size_buf_ub_arr mbeg_s, mend_s, mret_s;
	typename TT::node_seq mnode_h;
	typename TT::key_seq msep_h;
	parlay::sequence<size_type> mbeg_h, mend_h, mret_h;
	if (big) [[unlikely]] {
		mnode_h = TT::node_seq::uninitialized(merged_n);
		msep_h = TT::key_seq::uninitialized(merged_n);
		mbeg_h = parlay::sequence<size_type>::uninitialized(merged_n);
		mend_h = parlay::sequence<size_type>::uninitialized(merged_n);
		mret_h = parlay::sequence<size_type>(merged_n, 0);
	}
	node_type **mnode = big ? mnode_h.begin() : mnode_s.data();
	key_type *msep = big ? msep_h.begin() : msep_s.data();
	size_type *mbeg = big ? mbeg_h.begin() : mbeg_s.data();
	size_type *mend = big ? mend_h.begin() : mend_s.data();
	size_type *mret = big ? mret_h.begin() : mret_s.data();

	/* The merged child list; merged child w takes in[mbeg[w], mend[w]). */
	size_buf_ub_arr split_pos;
	partition_batch(split_pos, parlay::make_slice(in.keys + l, in.keys + r),
					interior);
	size_type size = interior->size;
	size_type w = 0;
	size_type q = 0;
	for (buf_size_type j = 0; j < m; ++j) {
		const size_type end = l + (j + 1 == m ? n : split_pos[j]);
		if (j > 0) {
			msep[w] = interior->split[j - 1];
		}
		mnode[w] = interior->tree_nodes[j];
		mbeg[w++] = l + (j == 0 ? 0 : split_pos[j - 1]);
		for (; q < own_n && own[q] < end; ++q) {
			const size_type p = own[q];
			mend[w - 1] = p;
			msep[w] = in.keys[p];
			mnode[w] = in.nodes[p];
			size += in.nodes[p]->size;
			mbeg[w++] = p + 1;
		}
		mend[w - 1] = end;
	}
	assert(w == merged_n && q == own_n);

	/* Shorter inputs go down where they route. */
	size_type final_n = merged_n;
	if (!big) {
		std::fill_n(mret, merged_n, 0);
	}
	if (n > own_n) {
		buf_size_buf_ub_arr touched_s;
		size_buf_ub_arr old_s;
		parlay::sequence<size_type> touched_h, old_h;
		if (big) [[unlikely]] {
			touched_h = parlay::sequence<size_type>::uninitialized(merged_n);
			old_h = parlay::sequence<size_type>::uninitialized(merged_n);
		}
		size_type touched_n = 0;
		auto touch = [&](size_type i) {
			if (big) {
				touched_h[touched_n++] = i;
			} else {
				touched_s[touched_n++] = static_cast<buf_size_type>(i);
			}
		};
		auto touched_at = [&](size_type t) -> size_type {
			return big ? touched_h[t] : touched_s[t];
		};
		size_type *old = big ? old_h.begin() : old_s.data();
		for (size_type i = 0; i < merged_n; ++i) {
			if (mbeg[i] < mend[i]) {
				touch(i);
			}
		}
		parlay::parallel_for(
			0, touched_n,
			[&](size_type t) {
				const size_type i = touched_at(t);
				old[t] = mnode[i]->size;
				auto const o = out.cut(2 * (mbeg[i] - l), 2 * (mend[i] - l));
				if (height == 1) { /* only entries reach a leaf */
					mret[i] = batch_insert_recursive(
						mnode[i],
						parlay::make_slice(in.entries + mbeg[i],
										   in.entries + mend[i]),
						o, leaf_entries_pattern, leaf_sort_pred);
				} else {
					mret[i] = graft_all_rec(
						mnode[i], height - 1, mbeg[i], mend[i], in, o,
						leaf_entries_pattern, leaf_sort_pred);
				}
			},
			1);
		for (size_type t = 0; t < touched_n; ++t) {
			const size_type i = touched_at(t);
			size -= old[t];
			if (mret[i] == 0) {
				size += mnode[i]->size;
				continue;
			}
			final_n += mret[i] - 1;
			auto const *o = out.begin() + 2 * (mbeg[i] - l);
			for (size_type u = 0; u < mret[i]; ++u) {
				size += o[u].first->size;
			}
		}
	}

	/* The children in order; one that split is replaced by its parts. */
	auto emit = [&](node_type **fnode, key_type *fsep) {
		size_type v = 0;
		for (size_type i = 0; i < merged_n; ++i) {
			if (i > 0) {
				fsep[v - 1] = msep[i];
			}
			if (mret[i] == 0) {
				fnode[v++] = mnode[i];
				continue;
			}
			auto const *o = out.begin() + 2 * (mbeg[i] - l);
			for (size_type t = 0; t < mret[i]; ++t) {
				if (t > 0) {
					fsep[v - 1] = o[t - 1].second;
				}
				fnode[v++] = o[t].first;
			}
		}
		assert(v == final_n);
	};

	if (final_n <= TT::k_buf_ub) {
		/* mnode and msep are copies, so node's arrays take the result. */
		emit(interior->tree_nodes.data(), interior->split.data());
		interior->node_n = static_cast<buf_size_type>(final_n);
		interior->size = size;
		interior->update_aug();
		return 0;
	}

	auto fnode = TT::node_seq::uninitialized(final_n);
	auto fsep = TT::key_seq::uninitialized(final_n - 1);
	emit(fnode.begin(), fsep.begin());
	free_node<interior_type>(node);
	const size_type bkt_n = generate_interior_nodes(
		parlay::make_slice(fnode), parlay::make_slice(fsep),
		gen_nodes_output_handler{
			.set_key = [&](size_type i, auto key) { out[i].second = key; },
			.set_node = [&](size_type i,
							auto child) { out[i].first = child; } });
	assert(bkt_n >= 2 && bkt_n <= out.size());
	return bkt_n;
}

template <typename TT>
typename btree<TT>::opt_key
btree<TT>::graft_detach_rec(node_type *node, opt_key const &lo,
							height_type height, graft_pool out)
{
	auto interior = static_cast<interior_type *>(node);
	const buf_size_type node_n = interior->get_node_n();
	assert(node_is_valid(node) && !node_has_leaf_children(node));
	assert(out.size() == interior->reinsert_size);

	boolean_buf_ub_arr drop;
	std::array<size_type, TT::k_buf_ub + 1> off;
	buf_size_buf_ub_arr todo;
	buf_size_type todo_n = 0;
	off[0] = 0;
	for (buf_size_type i = 0; i < node_n; ++i) {
		auto const *child =
			static_cast<interior_type const *>(interior->tree_nodes[i]);
		drop[i] = !child->valid;
		off[i + 1] = off[i] + child->reinsert_size;
		if (drop[i] || child->contain_invalid_subtree) {
			todo[todo_n++] = i;
		}
	}
	assert(off[node_n] == out.size());

	opt_key_buf_ub_arr raised;
	size_buf_ub_arr old;
	parlay::parallel_for(
		0, todo_n,
		[&](buf_size_type k) {
			const buf_size_type i = todo[k];
			auto *child = interior->tree_nodes[i];
			old[k] = child->size;
			opt_key const child_lo =
				i == 0 ? lo : opt_key(interior->split[i - 1]);
			auto const part = out.cut(off[i], off[i + 1]);
			if (drop[i]) {
				graft_decompose_rec(child, child_lo, height - 1, part);
			} else {
				raised[i] = graft_detach_rec(child, child_lo, height - 1, part);
			}
		},
		1);

	/* What node keeps: less what it dropped and what its children lost. */
	size_type size = interior->size;
	for (buf_size_type k = 0; k < todo_n; ++k) {
		size -= old[k];
		if (!drop[todo[k]]) {
			size += interior->tree_nodes[todo[k]]->size;
		}
	}
	return graft_compact(node, drop, raised, size);
}

template <typename TT>
void btree<TT>::graft_decompose_rec(node_type *node, opt_key const &lo,
									height_type height, graft_pool out)
{
	auto interior = static_cast<interior_type *>(node);
	const buf_size_type node_n = interior->get_node_n();
	assert(get_node_invalid_status(node));
	assert(out.size() == interior->reinsert_size);

	if (node_has_leaf_children(node)) {
		/*
		 * Balanced already: every leaf is valid and an item, or one
		 * underfull leaf is left and gives its entries. All empty: nothing.
		 */
		size_type w = 0;
		for (buf_size_type i = 0; i < node_n; ++i) {
			auto *leaf = static_cast<leaf_type *>(interior->tree_nodes[i]);
			if (node_is_valid(leaf)) {
				out.put(w++, leaf,
						i == 0 ? graft_bound(lo, leaf) : interior->split[i - 1],
						height - 1);
				continue;
			}
			auto const dst = out.entries.cut(w, w + leaf->size);
			std::copy_n(leaf->elements.begin(), leaf->size, dst.begin());
			if (!leaf->is_sorted) {
				std::sort(dst.begin(), dst.end(), key_less);
			}
			for (size_type k = 0; k < dst.size(); ++k) {
				out.put(w + k, nullptr, entry_traits::get_key(dst[k]),
						k_entry_source);
			}
			w += dst.size();
			free_node<leaf_type>(leaf);
		}
		assert(w == out.size());
		free_node<interior_type>(node);
		return;
	}

	/* A valid child takes one slot, then the inputs detached inside it. */
	std::array<size_type, TT::k_buf_ub + 1> off;
	off[0] = 0;
	for (buf_size_type i = 0; i < node_n; ++i) {
		auto const *child =
			static_cast<interior_type const *>(interior->tree_nodes[i]);
		off[i + 1] = off[i] + child->reinsert_size + (child->valid ? 1 : 0);
	}
	assert(off[node_n] == out.size());

	parlay::parallel_for(
		0, node_n,
		[&](buf_size_type i) {
			auto *child = static_cast<interior_type *>(interior->tree_nodes[i]);
			opt_key const child_lo =
				i == 0 ? lo : opt_key(interior->split[i - 1]);
			auto const part = out.cut(off[i], off[i + 1]);
			if (!child->valid) {
				graft_decompose_rec(child, child_lo, height - 1, part);
				return;
			}
			opt_key raised;
			if (child->contain_invalid_subtree) {
				raised = graft_detach_rec(child, child_lo, height - 1,
										  part.cut(1, part.size()));
			}
			key_type const bound =
				raised ? *raised : graft_bound(child_lo, child);
			/*
			 * Key order: what was cut from below its raised bound comes
			 * first. Usually nothing.
			 */
			size_type p = 0;
			for (; p + 1 < part.size() && TT::key_less(part.keys[p + 1], bound);
				 ++p) {
				part.move(p, p + 1);
			}
			part.put(p, child, bound, height - 1);
		},
		1);
	free_node<interior_type>(node);
}

template <typename TT>
typename btree<TT>::opt_key
btree<TT>::graft_compact(node_type *node, boolean_buf_ub_arr const &drop,
						 opt_key_buf_ub_arr const &raised, size_type size)
{
	auto interior = static_cast<interior_type *>(node);
	const buf_size_type node_n = interior->get_node_n();
	opt_key first;
	buf_size_type w = 0;
	for (buf_size_type i = 0; i < node_n; ++i) {
		if (drop[i]) {
			continue;
		}
		/* The kept child's lower bound: raised, or its own separator. */
		opt_key const bound =
			raised[i] ?
				raised[i] :
				(i > 0 ? opt_key(interior->split[i - 1]) : std::nullopt);
		if (w == 0) {
			if (i > 0 || raised[i]) {
				first = bound;
			}
		} else {
			interior->split[w - 1] = *bound;
		}
		interior->tree_nodes[w++] = interior->tree_nodes[i];
	}
	assert(w > 0);
	interior->node_n = w;
	interior->size = size;
	interior->update_aug();
	interior->reset_deletion_flag();
	return first;
}

template <typename TT>
TT::size_type
btree<TT>::graft_rec(node_type *node, height_type height, height_type target,
					 TT::node_slice items, TT::const_key_slice bounds,
					 node_split_pair_slice out, opt_key const &edge)
{
	assert(items.size() == bounds.size() && 2 * items.size() == out.size());
	if (height == target + 1) {
		return graft_into_node(node, items, bounds, out, edge);
	}

	auto interior = static_cast<interior_type *>(node);
	const size_type n = items.size();
	size_buf_ub_arr split_pos;
	partition_batch(split_pos, bounds, interior);

	size_buf_ub_arr ret;
	buf_size_buf_ub_arr nodes_map;
	buf_size_type cnt = 0;
	for (buf_size_type i = 0; i < interior->get_node_n(); ++i) {
		const size_type l = i == 0 ? 0 : split_pos[i - 1];
		const size_type r = i == interior->get_node_n() - 1 ? n : split_pos[i];
		if (l != r) {
			nodes_map[cnt++] = i;
		} else {
			ret[i] = 0;
		}
	}

	parlay::parallel_for(
		0, cnt,
		[&](buf_size_type k) {
			const buf_size_type i = nodes_map[k];
			const size_type l = i == 0 ? 0 : split_pos[i - 1];
			const size_type r =
				i == interior->get_node_n() - 1 ? n : split_pos[i];
			ret[i] = graft_rec(interior->tree_nodes[i], height - 1, target,
							   items.cut(l, r), bounds.cut(l, r),
							   out.cut(2 * l, 2 * r), edge);
		},
		1);

	size_type new_node_n = interior->get_node_n();
	for (buf_size_type k = 0; k < cnt; ++k) {
		if (ret[nodes_map[k]] > 0) {
			new_node_n += ret[nodes_map[k]] - 1;
		}
	}
	if (new_node_n <= TT::k_buf_ub) {
		batch_insert_update_interior(node, out, split_pos, new_node_n, ret);
		return 0;
	}
	return handling_interior_split_overflow(node, out, split_pos, new_node_n,
											ret);
}

template <typename TT>
TT::size_type btree<TT>::graft_into_node(node_type *node, TT::node_slice items,
										 TT::const_key_slice bounds,
										 node_split_pair_slice out,
										 opt_key const &edge)
{
	auto interior = static_cast<interior_type *>(node);
	const buf_size_type node_n = interior->get_node_n();
	const size_type g = items.size();
	const size_type n = node_n + g;

	/*
	 * Items below edge lead. Every other item goes right after the child
	 * its bound routes to, with the bound as separator: the key range of
	 * a removed node went to its left neighbour.
	 */
	auto merge = [&](auto nodes, auto keys) {
		size_type w = 0;
		size_type k = 0;
		size_type added = 0;
		if (edge) {
			while (k < g && TT::key_less(bounds[k], *edge)) {
				if (w > 0) {
					keys[w - 1] = bounds[k];
				}
				added += items[k]->size;
				nodes[w++] = items[k++];
			}
		}
		for (buf_size_type j = 0; j < node_n; ++j) {
			if (w > 0) {
				keys[w - 1] = j == 0 ? *edge : interior->split[j - 1];
			}
			nodes[w++] = interior->tree_nodes[j];
			while (k < g && (j + 1 == node_n ||
							 TT::key_less(bounds[k], interior->split[j]))) {
				keys[w - 1] = bounds[k];
				added += items[k]->size;
				nodes[w++] = items[k++];
			}
		}
		assert(w == n && k == g);
		return added;
	};

	if (n <= TT::k_buf_ub) {
		typename interior_type::node_arr nodes;
		typename interior_type::split_arr keys;
		const size_type added = merge(nodes.begin(), keys.begin());
		std::copy_n(nodes.begin(), n, interior->tree_nodes.begin());
		std::copy_n(keys.begin(), n - 1, interior->split.begin());
		interior->node_n = static_cast<buf_size_type>(n);
		interior->size += added;
		interior->update_aug();
		return 0;
	}

	auto nodes = TT::node_seq::uninitialized(n);
	auto keys = TT::key_seq::uninitialized(n - 1);
	merge(nodes.begin(), keys.begin());
	free_node<interior_type>(node);
	const size_type bkt_n = generate_interior_nodes(
		parlay::make_slice(nodes), parlay::make_slice(keys),
		gen_nodes_output_handler{
			.set_key = [&](size_type i, auto key) { out[i].second = key; },
			.set_node = [&](size_type i,
							auto child) { out[i].first = child; } });
	assert(bkt_n >= 2 && bkt_n <= out.size());
	return bkt_n;
}

template <typename TT>
TT::key_type btree<TT>::graft_bound(opt_key const &lo, node_type const *node)
{
	if (lo) {
		return *lo;
	}
	while (!node->is_leaf) {
		node = static_cast<interior_type const *>(node)->tree_nodes[0];
	}
	auto const *leaf = static_cast<leaf_type const *>(node);
	assert(leaf->size > 0);
	auto const it = std::min_element(
		leaf->elements.begin(), leaf->elements.begin() + leaf->size,
		[](entry_type const &a, entry_type const &b) {
			return TT::entry_less(a, b);
		});
	return entry_traits::get_key(*it);
}

template <typename TT>
TT::height_type btree<TT>::graft_height(node_type const *node)
{
	height_type h = 0;
	for (; !node->is_leaf; ++h) {
		node = static_cast<interior_type const *>(node)->tree_nodes[0];
	}
	return h;
}

} /* namespace pab_tree */

#endif /* PABTREE_B_TREE_IMPL_BATCH_OP_B_BATCH_DELETE_GRAFT_HPP_ */

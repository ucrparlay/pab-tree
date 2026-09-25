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
 * graft_delete. Below a valid root, each tomb (the highest valid node with
 * an invalid child) cuts the largest valid subtrees out of its invalid
 * subtrees and grafts them back whole, tallest first. Underfull leaves go
 * back entry by entry. docs/plan_graft_delete_2026-09.md.
 */

namespace pab_tree
{
template <typename TT>
template <typename LeafSortPred>
void btree<TT>::graft_repair(const TT::pattern_type leaf_entries_pattern,
							 LeafSortPred const &leaf_sort_pred)
{
	assert(!this->root_->is_leaf && node_is_valid(this->root_));
	const height_type height = graft_height(this->root_);

	graft_settle_rec(this->root_, leaf_entries_pattern, leaf_sort_pred);
	if (!node_contain_invalid_subtree(this->root_)) {
		return;
	}

	auto const up = graft_tomb_rec(this->root_, std::nullopt, height,
								   leaf_entries_pattern, leaf_sort_pred);
	if (!up.empty()) { /* the root split: new levels on top */
		auto nodes =
			parlay::tabulate(up.size(), [&](size_t i) { return up[i].first; });
		auto keys = parlay::tabulate(up.size() - 1,
									 [&](size_t i) { return up[i].second; });
		this->root_ = assemble_interior_levels(parlay::make_slice(nodes),
											   parlay::make_slice(keys));
	}

#ifdef DEBUG
	[[maybe_unused]] const bool tree_ok = this->validate<true, false>();
	assert(tree_ok);
#endif /* DEBUG */
}

template <typename TT>
template <typename LeafSortPred>
void btree<TT>::graft_settle_rec(node_type *node,
								 const TT::pattern_type leaf_entries_pattern,
								 LeafSortPred const &leaf_sort_pred)
{
	assert(!node->is_leaf);
	auto interior = static_cast<interior_type *>(node);
	const bool is_root = node == this->root_;

	if (node_has_leaf_children(node)) {
		size_type total = 0;
		size_type underfull_n = 0;
		size_type underfull_sum = 0;
		for (buf_size_type i = 0; i < interior->get_node_n(); ++i) {
			const size_type sz = interior->tree_nodes[i]->size;
			total += sz;
			if (sz < TT::k_buf_lb) {
				underfull_n++;
				underfull_sum += sz;
			}
		}
		/* All empty: left whole, graft_decompose_rec frees it. */
		if (total > 0 && underfull_n > 0 && interior->get_node_n() >= 2) {
			balance_over_leaves(node, underfull_sum, leaf_entries_pattern,
								leaf_sort_pred);
		}

		buf_size_type valid_n = 0;
		for (buf_size_type i = 0; i < interior->get_node_n(); ++i) {
			valid_n += node_is_valid(interior->tree_nodes[i]) ? 1 : 0;
		}
		interior->valid = is_root ? valid_n > 0 : valid_n >= TT::k_buf_lb;
		/* Balanced: a valid leaf-parent has no underfull leaf left. */
		assert(!interior->valid || valid_n == interior->get_node_n());
		interior->contain_invalid_subtree = !interior->valid;
		/* An invalid leaf-parent hands over every leaf. */
		interior->reinsert_size =
			interior->valid || total == 0 ? 0 : interior->get_node_n();
		return;
	}

	typename interior_type::node_arr todo;
	buf_size_type todo_n = 0;
	for (buf_size_type i = 0; i < interior->get_node_n(); ++i) {
		if (node_contain_invalid_subtree(interior->tree_nodes[i])) {
			todo[todo_n++] = interior->tree_nodes[i];
		}
	}
	parlay::parallel_for(
		0, todo_n,
		[&](buf_size_type i) {
			graft_settle_rec(todo[i], leaf_entries_pattern, leaf_sort_pred);
		},
		1);

	buf_size_type valid_n = 0;
	bool contain = false;
	size_type item_n = 0;
	for (buf_size_type i = 0; i < interior->get_node_n(); ++i) {
		auto const *child =
			static_cast<interior_type const *>(interior->tree_nodes[i]);
		valid_n += child->valid ? 1 : 0;
		contain = contain || child->contain_invalid_subtree;
		item_n += child->reinsert_size;
	}
	interior->valid = is_root ? valid_n > 0 : valid_n >= TT::k_buf_lb;
	interior->contain_invalid_subtree = !interior->valid || contain;
	/* An invalid node also hands over its valid children, whole. */
	interior->reinsert_size = item_n + (interior->valid ? 0 : valid_n);
	interior->update_aug();
}

template <typename TT>
template <typename LeafSortPred>
typename btree<TT>::node_split_pair_seq
btree<TT>::graft_tomb_rec(node_type *node, opt_key const &lo,
						  height_type height,
						  const TT::pattern_type leaf_entries_pattern,
						  LeafSortPred const &leaf_sort_pred)
{
	auto interior = static_cast<interior_type *>(node);
	const buf_size_type node_n = interior->get_node_n();
	assert(node_is_valid(node) && node_contain_invalid_subtree(node));
	assert(!node_has_leaf_children(node));

	for (buf_size_type i = 0; i < node_n; ++i) {
		if (get_node_invalid_status(interior->tree_nodes[i])) {
			return graft_reinsert(node, lo, height, leaf_entries_pattern,
								  leaf_sort_pred);
		}
	}

	parlay::sequence<node_split_pair_seq> up(node_n);
	buf_size_buf_ub_arr todo;
	buf_size_type todo_n = 0;
	for (buf_size_type i = 0; i < node_n; ++i) {
		if (node_contain_invalid_subtree(interior->tree_nodes[i])) {
			todo[todo_n++] = i;
		}
	}
	parlay::parallel_for(
		0, todo_n,
		[&](buf_size_type k) {
			const buf_size_type i = todo[k];
			up[i] = graft_tomb_rec(
				interior->tree_nodes[i],
				i == 0 ? lo : opt_key(interior->split[i - 1]), height - 1,
				leaf_entries_pattern, leaf_sort_pred);
		},
		1);
	return graft_splice(node, up);
}

template <typename TT>
typename btree<TT>::node_split_pair_seq
btree<TT>::graft_splice(node_type *node,
						parlay::sequence<node_split_pair_seq> const &up)
{
	auto interior = static_cast<interior_type *>(node);
	const buf_size_type node_n = interior->get_node_n();

	size_type n = 0;
	for (buf_size_type i = 0; i < node_n; ++i) {
		n += up[i].empty() ? 1 : up[i].size();
	}

	if (n == node_n) { /* no child split */
		size_type size = 0;
		for (buf_size_type i = 0; i < node_n; ++i) {
			size += interior->tree_nodes[i]->size;
		}
		interior->size = size;
		interior->update_aug();
		interior->reset_deletion_flag();
		return {};
	}

	auto nodes = TT::node_seq::uninitialized(n);
	auto keys = TT::key_seq::uninitialized(n - 1);
	size_type w = 0;
	for (buf_size_type i = 0; i < node_n; ++i) {
		if (up[i].empty()) {
			nodes[w++] = interior->tree_nodes[i];
		} else {
			for (size_type j = 0; j < up[i].size(); ++j) {
				if (j > 0) {
					keys[w - 1] = up[i][j - 1].second;
				}
				nodes[w++] = up[i][j].first;
			}
		}
		if (i + 1 < node_n) {
			keys[w - 1] = interior->split[i];
		}
	}
	assert(w == n);

	if (n <= TT::k_buf_ub) {
		size_type size = 0;
		for (size_type i = 0; i < n; ++i) {
			interior->tree_nodes[i] = nodes[i];
			size += nodes[i]->size;
		}
		std::copy_n(keys.begin(), n - 1, interior->split.begin());
		interior->node_n = static_cast<buf_size_type>(n);
		interior->size = size;
		interior->update_aug();
		interior->reset_deletion_flag();
		return {};
	}

	free_node<interior_type>(node);
	auto out = node_split_pair_seq::uninitialized(get_bkt_num(n));
	[[maybe_unused]] const size_type bkt_n = generate_interior_nodes(
		parlay::make_slice(nodes), parlay::make_slice(keys),
		gen_nodes_output_handler{
			.set_key = [&](size_type i, auto key) { out[i].second = key; },
			.set_node = [&](size_type i,
							auto child) { out[i].first = child; } });
	assert(bkt_n == out.size());
	return out;
}

template <typename TT>
template <typename LeafSortPred>
typename btree<TT>::node_split_pair_seq
btree<TT>::graft_reinsert(node_type *tomb, opt_key const &lo,
						  height_type height,
						  const TT::pattern_type leaf_entries_pattern,
						  LeafSortPred const &leaf_sort_pred)
{
	auto interior = static_cast<interior_type *>(tomb);
	const size_type item_n = interior->reinsert_size;

	auto pool_nodes = TT::node_seq::uninitialized(item_n);
	auto pool_bounds = TT::key_seq::uninitialized(item_n);
	auto pool_heights = parlay::sequence<height_type>::uninitialized(item_n);
	graft_pool const pool{ parlay::make_slice(pool_nodes),
						   parlay::make_slice(pool_bounds),
						   parlay::make_slice(pool_heights) };

	/* Items left of all of tomb's content, when its first child went. */
	opt_key edge = graft_detach_rec(tomb, lo, height, pool);

	/* tomb's children, as a list that may outgrow k_buf_ub. */
	auto kids = typename TT::node_seq(interior->tree_nodes.begin(),
									  interior->tree_nodes.begin() +
										  interior->get_node_n());
	auto seps =
		typename TT::key_seq(interior->split.begin(),
							 interior->split.begin() + interior->get_split_n());

	/* start[j]: the first of n sorted keys at or above kid j's bound. */
	auto route = [&](size_type n, auto const &key_at) {
		parlay::sequence<size_type> start(kids.size() + 1);
		start[0] = 0;
		start[kids.size()] = n;
		parlay::parallel_for(1, kids.size(), [&](size_t j) {
			size_type l = 0;
			size_type r = n;
			while (l < r) {
				const size_type m = l + (r - l) / 2;
				if (TT::key_less(key_at(m), seps[j - 1])) {
					l = m + 1;
				} else {
					r = m;
				}
			}
			start[j] = l;
		});
		return start;
	};

	/* Kid j is replaced by the ret[j] nodes at out[2 * start[j]] on. */
	auto splice_kids = [&](parlay::sequence<size_type> const &ret,
						   parlay::sequence<size_type> const &start,
						   node_split_pair_seq const &out) {
		size_type n = 0;
		for (size_t j = 0; j < kids.size(); ++j) {
			n += ret[j] == 0 ? 1 : ret[j];
		}
		if (n == kids.size()) {
			return;
		}
		auto new_kids = TT::node_seq::uninitialized(n);
		auto new_seps = TT::key_seq::uninitialized(n - 1);
		size_type w = 0;
		for (size_t j = 0; j < kids.size(); ++j) {
			if (ret[j] == 0) {
				new_kids[w++] = kids[j];
			} else {
				auto const *o = out.begin() + 2 * start[j];
				for (size_type t = 0; t < ret[j]; ++t) {
					if (t > 0) {
						new_seps[w - 1] = o[t - 1].second;
					}
					new_kids[w++] = o[t].first;
				}
			}
			if (j + 1 < kids.size()) {
				new_seps[w - 1] = seps[j];
			}
		}
		assert(w == n);
		kids = std::move(new_kids);
		seps = std::move(new_seps);
	};

	/* Tallest first: every node at the target's child height is original. */
	for (height_type h = height - 1; h-- > 0;) {
		auto const idx =
			parlay::filter(parlay::iota<size_t>(item_n),
						   [&](size_t i) { return pool_heights[i] == h; });
		if (idx.empty()) {
			continue;
		}
		auto items = parlay::tabulate(
			idx.size(), [&](size_t i) { return pool_nodes[idx[i]]; });
		auto const bounds = parlay::tabulate(
			idx.size(), [&](size_t i) { return pool_bounds[idx[i]]; });
		auto const start =
			route(idx.size(), [&](size_type i) { return bounds[i]; });

		auto out = node_split_pair_seq::uninitialized(2 * idx.size());
		parlay::sequence<size_type> ret(kids.size(), 0);
		parlay::parallel_for(
			0, kids.size(),
			[&](size_t j) {
				const size_type l = start[j];
				const size_type r = start[j + 1];
				if (l == r) {
					return;
				}
				ret[j] = graft_rec(
					kids[j], height - 1, h, parlay::make_slice(items).cut(l, r),
					parlay::make_slice(bounds.cbegin(), bounds.cend())
						.cut(l, r),
					parlay::make_slice(out).cut(2 * l, 2 * r), edge);
			},
			1);
		splice_kids(ret, start, out);

		if (edge && TT::key_less(bounds[0], *edge)) {
			edge = bounds[0];
		}
	}

	/* The underfull leaves' entries, sorted, back through batch insert. */
	auto const src =
		parlay::filter(parlay::iota<size_t>(item_n), [&](size_t i) {
			return pool_heights[i] == k_entry_source;
		});
	if (!src.empty()) {
		auto const sizes = parlay::tabulate(src.size(), [&](size_t i) {
			return static_cast<size_t>(pool_nodes[src[i]]->size);
		});
		auto const [offsets, total] = parlay::scan(sizes);
		auto entries = TT::entry_seq::uninitialized(total);
		parlay::parallel_for(0, src.size(), [&](size_t i) {
			auto *leaf = pool_nodes[src[i]];
			extract_points_in_leaf<entry_slice, true>(
				leaf, parlay::make_slice(entries).cut(offsets[i],
													  offsets[i] + sizes[i]));
			free_node<leaf_type>(leaf);
		});
		assert(parlay::is_sorted(entries,
								 [](entry_type const &a, entry_type const &b) {
									 return !TT::entry_less(b, a);
								 }));

		if (total > 0) {
			auto const in =
				parlay::make_slice(entries.cbegin(), entries.cend());
			auto const start = route(total, [&](size_type i) {
				return entry_traits::get_key(in[i]);
			});
			auto out = node_split_pair_seq::uninitialized(2 * total);
			parlay::sequence<size_type> ret(kids.size(), 0);
			parlay::parallel_for(
				0, kids.size(),
				[&](size_t j) {
					const size_type l = start[j];
					const size_type r = start[j + 1];
					if (l == r) {
						return;
					}
					ret[j] = batch_insert_recursive(
						kids[j], in.cut(l, r),
						parlay::make_slice(out).cut(2 * l, 2 * r),
						leaf_entries_pattern, leaf_sort_pred);
				},
				1);
			splice_kids(ret, start, out);
		}
	}

	if (kids.size() <= TT::k_buf_ub) {
		size_type size = 0;
		for (size_t j = 0; j < kids.size(); ++j) {
			interior->tree_nodes[j] = kids[j];
			size += kids[j]->size;
		}
		std::copy(seps.begin(), seps.end(), interior->split.begin());
		interior->node_n = static_cast<buf_size_type>(kids.size());
		interior->size = size;
		interior->update_aug();
		interior->reset_deletion_flag();
		return {};
	}

	/* tomb overflowed: regroup its children, its parent splices them. */
	free_node<interior_type>(tomb);
	auto up = node_split_pair_seq::uninitialized(get_bkt_num(kids.size()));
	[[maybe_unused]] const size_type bkt_n = generate_interior_nodes(
		parlay::make_slice(kids), parlay::make_slice(seps),
		gen_nodes_output_handler{
			.set_key = [&](size_type i, auto key) { up[i].second = key; },
			.set_node = [&](size_type i,
							auto child) { up[i].first = child; } });
	assert(bkt_n == up.size());
	return up;
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
	parlay::parallel_for(
		0, todo_n,
		[&](buf_size_type k) {
			const buf_size_type i = todo[k];
			auto *child = interior->tree_nodes[i];
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
	return graft_compact(node, drop, raised);
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
		/* Every leaf is an item, unless all are empty. */
		assert(out.size() == 0 || out.size() == node_n);
		for (buf_size_type i = 0; i < node_n; ++i) {
			auto *leaf = interior->tree_nodes[i];
			if (out.size() == 0) {
				free_node<leaf_type>(leaf);
				continue;
			}
			out.nodes[i] = leaf;
			if (node_is_valid(leaf)) {
				out.bounds[i] =
					i == 0 ? graft_bound(lo, leaf) : interior->split[i - 1];
				out.heights[i] = height - 1;
			} else {
				out.heights[i] = k_entry_source;
			}
		}
		free_node<interior_type>(node);
		return;
	}

	/* A valid child takes one slot, then the items detached inside it. */
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
			if (!child->valid) {
				graft_decompose_rec(child, child_lo, height - 1,
									out.cut(off[i], off[i + 1]));
				return;
			}
			opt_key raised;
			if (child->contain_invalid_subtree) {
				raised = graft_detach_rec(child, child_lo, height - 1,
										  out.cut(off[i] + 1, off[i + 1]));
			}
			out.nodes[off[i]] = child;
			out.bounds[off[i]] =
				raised ? *raised : graft_bound(child_lo, child);
			out.heights[off[i]] = height - 1;
		},
		1);
	free_node<interior_type>(node);
}

template <typename TT>
typename btree<TT>::opt_key
btree<TT>::graft_compact(node_type *node, boolean_buf_ub_arr const &drop,
						 opt_key_buf_ub_arr const &raised)
{
	auto interior = static_cast<interior_type *>(node);
	const buf_size_type node_n = interior->get_node_n();
	opt_key first;
	buf_size_type w = 0;
	size_type size = 0;
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
		size += interior->tree_nodes[w - 1]->size;
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

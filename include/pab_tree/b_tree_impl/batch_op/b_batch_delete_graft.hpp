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
 * leaves and counted the items; one pass cuts the largest valid subtrees
 * out of every invalid subtree into one pool, and one descent puts them
 * back whole, with the underfull leaves' entries, into the tree that is
 * left. docs/plan_graft_delete_2026-09.md.
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
	const size_type item_n =
		static_cast<interior_type *>(this->root_)->reinsert_size;
	const height_type levels = graft_height(this->root_);

	/* 1. Cut every invalid subtree into one pool, sized by the mark pass. */
	auto pool_nodes = TT::node_seq::uninitialized(item_n);
	auto pool_bounds = TT::key_seq::uninitialized(item_n);
	auto pool_heights = parlay::sequence<height_type>::uninitialized(item_n);
	opt_key const edge =
		graft_detach_rec(this->root_, std::nullopt, levels,
						 graft_pool{ parlay::make_slice(pool_nodes),
									 parlay::make_slice(pool_bounds),
									 parlay::make_slice(pool_heights) });
	if (item_n == 0) {
		return;
	}

	/* 2. Items and the underfull leaves' entries, sorted by key. */
	struct input {
		key_type key;
		height_type height;
		node_type *node;
		entry_type entry;
	};
	auto const src =
		parlay::filter(parlay::iota<size_type>(item_n), [&](size_type i) {
			return pool_heights[i] == k_entry_source;
		});
	auto const sub =
		parlay::filter(parlay::iota<size_type>(item_n), [&](size_type i) {
			return pool_heights[i] != k_entry_source;
		});
	auto const sizes = parlay::tabulate(src.size(), [&](size_t i) {
		return static_cast<size_type>(pool_nodes[src[i]]->size);
	});
	auto const [offsets, entry_n] = parlay::scan(sizes);
	const size_type n = sub.size() + entry_n;
	auto inputs = parlay::sequence<input>::uninitialized(n);
	parlay::parallel_for(0, sub.size(), [&](size_t i) {
		auto const p = sub[i];
		inputs[i] = input{ pool_bounds[p], pool_heights[p], pool_nodes[p],
						   entry_type{} };
	});
	parlay::parallel_for(0, src.size(), [&](size_t i) {
		auto *leaf = static_cast<leaf_type *>(pool_nodes[src[i]]);
		auto *dst = inputs.begin() + sub.size() + offsets[i];
		for (size_type k = 0; k < leaf->size; ++k) {
			auto const &e = leaf->elements[k];
			dst[k] =
				input{ entry_traits::get_key(e), k_entry_source, nullptr, e };
		}
		free_node<leaf_type>(leaf);
	});
	/* Keys are distinct; on a tie the taller input would go first. */
	parlay::sort_inplace(inputs, [](input const &a, input const &b) {
		if (TT::key_less(a.key, b.key) || TT::key_less(b.key, a.key)) {
			return TT::key_less(a.key, b.key);
		}
		auto rank = [](height_type h) {
			return h == k_entry_source ? -1 : static_cast<int>(h);
		};
		return rank(a.height) > rank(b.height);
	});
	auto const keys =
		parlay::tabulate(n, [&](size_t i) { return inputs[i].key; });
	auto const nodes =
		parlay::tabulate(n, [&](size_t i) { return inputs[i].node; });
	auto const entries =
		parlay::tabulate(n, [&](size_t i) { return inputs[i].entry; });
	parlay::sequence<parlay::sequence<size_type>> hpos(levels);
	for (height_type h = 0; h < levels; ++h) {
		hpos[h] = parlay::filter(parlay::iota<size_type>(n), [&](size_type i) {
			return inputs[i].height == h;
		});
	}

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
		opt_key e = edge;
		for (height_type h = levels; h-- > 0;) {
			auto const &hp = hpos[h];
			const size_type c = static_cast<size_type>(
				std::lower_bound(hp.begin(), hp.end(), first) - hp.begin());
			if (c == 0) {
				continue;
			}
			auto items =
				parlay::tabulate(c, [&](size_t i) { return nodes[hp[i]]; });
			auto const bounds =
				parlay::tabulate(c, [&](size_t i) { return keys[hp[i]]; });
			auto const o = parlay::make_slice(scratch).cut(0, 2 * c);
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
		auto const lead =
			parlay::filter(parlay::iota<size_type>(first), [&](size_type i) {
				return inputs[i].height == k_entry_source;
			});
		if (!lead.empty()) {
			auto const lead_entries = parlay::tabulate(
				lead.size(), [&](size_t i) { return entries[lead[i]]; });
			auto const o = parlay::make_slice(scratch).cut(0, 2 * lead.size());
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
		graft_batch const in{ keys.begin(), nodes.begin(), entries.begin(),
							  hpos.begin(), static_cast<size_type>(levels) };
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
 * One descent for in[l, r), all inside node's range and sorted by key.
 * Items one level below node join its child list first, each right after
 * the child its bound routes to; everything shorter then routes by the
 * merged list, so it can land inside a just-joined item. node is rebuilt
 * once, after its children returned. Buffers are on the stack; the heap is
 * used only when node takes more than k_buf_ub children and splits.
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
	auto interior = static_cast<interior_type *>(node);
	const buf_size_type m = interior->get_node_n();

	/* The first position in [lo, r) whose key is not below k. */
	auto first_at = [&](size_type lo, key_type const &k) {
		size_type hi = r;
		while (lo < hi) {
			const size_type mid = lo + (hi - lo) / 2;
			if (TT::key_less(in.keys[mid], k)) {
				lo = mid + 1;
			} else {
				hi = mid;
			}
		}
		return lo;
	};

	/* The items that become node's children: one level below it. */
	size_type const *own = nullptr;
	size_type own_n = 0;
	if (static_cast<size_type>(height - 1) < in.level_n) {
		auto const &hp = in.hpos[height - 1];
		own = std::lower_bound(hp.begin(), hp.end(), l);
		own_n =
			static_cast<size_type>(std::lower_bound(own, hp.end(), r) - own);
	}
	const size_type merged_n = m + own_n;

	const bool big = merged_n > TT::k_buf_ub;
	typename interior_type::node_arr mnode_s;
	typename interior_type::split_arr msep_s;
	size_buf_ub_arr mbeg_s, mend_s, mret_s, touched_s;
	typename TT::node_seq mnode_h;
	typename TT::key_seq msep_h;
	parlay::sequence<size_type> mbeg_h, mend_h, mret_h, touched_h;
	if (big) [[unlikely]] {
		mnode_h = TT::node_seq::uninitialized(merged_n);
		msep_h = TT::key_seq::uninitialized(merged_n);
		mbeg_h = parlay::sequence<size_type>::uninitialized(merged_n);
		mend_h = parlay::sequence<size_type>::uninitialized(merged_n);
		mret_h = parlay::sequence<size_type>::uninitialized(merged_n);
		touched_h = parlay::sequence<size_type>::uninitialized(merged_n);
	}
	node_type **mnode = big ? mnode_h.begin() : mnode_s.data();
	key_type *msep = big ? msep_h.begin() : msep_s.data();
	size_type *mbeg = big ? mbeg_h.begin() : mbeg_s.data();
	size_type *mend = big ? mend_h.begin() : mend_s.data();
	size_type *mret = big ? mret_h.begin() : mret_s.data();
	size_type *touched = big ? touched_h.begin() : touched_s.data();

	/*
	 * The merged child list; child w takes in[mbeg[w], mend[w]). A removed
	 * node's range went to its left neighbour, so an item goes right after
	 * the child its bound routes to, and takes the rest of that range.
	 */
	size_type w = 0;
	size_type q = 0;
	size_type start = l;
	for (buf_size_type j = 0; j < m; ++j) {
		const size_type end =
			j + 1 == m ? r : first_at(start, interior->split[j]);
		if (j > 0) {
			mend[w - 1] = start;
			msep[w] = interior->split[j - 1];
		}
		mnode[w] = interior->tree_nodes[j];
		mbeg[w] = start;
		++w;
		for (; q < own_n && own[q] < end; ++q) {
			const size_type p = own[q];
			mend[w - 1] = p;
			msep[w] = in.keys[p];
			mnode[w] = in.nodes[p];
			mbeg[w] = p + 1;
			++w;
		}
		start = end;
	}
	mend[w - 1] = r;
	assert(w == merged_n && q == own_n);

	size_type touched_n = 0;
	for (size_type i = 0; i < merged_n; ++i) {
		mret[i] = 0;
		if (mbeg[i] < mend[i]) {
			touched[touched_n++] = i;
		}
	}
	parlay::parallel_for(
		0, touched_n,
		[&](size_type t) {
			const size_type i = touched[t];
			auto const o = out.cut(2 * (mbeg[i] - l), 2 * (mend[i] - l));
			if (height == 1) { /* only entries reach a leaf */
				mret[i] = batch_insert_recursive(
					mnode[i],
					parlay::make_slice(in.entries + mbeg[i],
									   in.entries + mend[i]),
					o, leaf_entries_pattern, leaf_sort_pred);
			} else {
				mret[i] =
					graft_all_rec(mnode[i], height - 1, mbeg[i], mend[i], in, o,
								  leaf_entries_pattern, leaf_sort_pred);
			}
		},
		1);

	size_type final_n = merged_n;
	for (size_type i = 0; i < merged_n; ++i) {
		if (mret[i] > 0) {
			final_n += mret[i] - 1;
		}
	}

	/* The children in order; one that split is replaced by its parts. */
	auto emit = [&](node_type **fnode, key_type *fsep) {
		size_type v = 0;
		size_type size = 0;
		for (size_type i = 0; i < merged_n; ++i) {
			if (i > 0) {
				fsep[v - 1] = msep[i];
			}
			if (mret[i] == 0) {
				size += mnode[i]->size;
				fnode[v++] = mnode[i];
				continue;
			}
			auto const *o = out.begin() + 2 * (mbeg[i] - l);
			for (size_type t = 0; t < mret[i]; ++t) {
				if (t > 0) {
					fsep[v - 1] = o[t - 1].second;
				}
				size += o[t].first->size;
				fnode[v++] = o[t].first;
			}
		}
		assert(v == final_n);
		return size;
	};

	if (final_n <= TT::k_buf_ub) {
		typename interior_type::node_arr fnode;
		typename interior_type::split_arr fsep;
		const size_type size = emit(fnode.data(), fsep.data());
		std::copy_n(fnode.begin(), final_n, interior->tree_nodes.begin());
		std::copy_n(fsep.begin(), final_n - 1, interior->split.begin());
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

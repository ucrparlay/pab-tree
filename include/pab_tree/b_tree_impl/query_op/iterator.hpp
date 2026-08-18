#ifndef PABTREE_B_TREE_IMPL_QUERY_OP_ITERATOR_HPP_
#define PABTREE_B_TREE_IMPL_QUERY_OP_ITERATOR_HPP_

#include <algorithm>
#include <cassert>
#include <utility>

#include <parlay/parallel.h>

#include "pab_tree/b_tree.h"

namespace pab_tree
{

template <typename TT>
void btree<TT>::ensure_leaf_sorted(leaf_type *leaf)
{
	if (!leaf->is_sorted) {
		std::sort(leaf->elements.begin(), leaf->elements.begin() + leaf->size,
				  key_less);
		leaf->is_sorted = true;
	}
}

template <typename TT>
void btree<TT>::sort_leaves_rec(node_type *node)
{
	if (node->is_leaf) {
		ensure_leaf_sorted(static_cast<leaf_type *>(node));
		return;
	}

	auto interior = static_cast<interior_type *>(node);
	auto const node_n = interior->get_node_n();
	parlay::parallel_for(
		0, node_n,
		[&](buf_size_type i) { sort_leaves_rec(interior->tree_nodes[i]); }, 1);
}

template <typename TT>
void btree<TT>::sort_leaves() const
{
	if (this->root_ == nullptr) [[unlikely]] {
		return;
	}
	sort_leaves_rec(this->root_);
}

template <typename TT>
auto btree<TT>::leftmost_leaf() const -> leaf_type *
{
	if (this->root_ == nullptr) [[unlikely]] {
		return nullptr;
	}

	node_type *node = this->root_;
	while (!node->is_leaf) {
		node = static_cast<interior_type *>(node)->tree_nodes[0];
	}
	auto leaf = static_cast<leaf_type *>(node);
	ensure_leaf_sorted(leaf);
	return leaf;
}

template <typename TT>
auto btree<TT>::rightmost_leaf() const -> leaf_type *
{
	if (this->root_ == nullptr) [[unlikely]] {
		return nullptr;
	}

	node_type *node = this->root_;
	while (!node->is_leaf) {
		auto interior = static_cast<interior_type *>(node);
		node = interior->tree_nodes[interior->get_node_n() - 1];
	}
	auto leaf = static_cast<leaf_type *>(node);
	ensure_leaf_sorted(leaf);
	return leaf;
}

template <typename TT>
auto btree<TT>::leaf_covering(key_type const &k) const -> leaf_type *
{
	if (this->root_ == nullptr) [[unlikely]] {
		return nullptr;
	}

	node_type *node = this->root_;
	while (!node->is_leaf) {
		auto interior = static_cast<interior_type *>(node);
		node = interior->tree_nodes[interior->get_node_id_containing_key(k)];
	}
	auto leaf = static_cast<leaf_type *>(node);
	ensure_leaf_sorted(leaf);
	return leaf;
}

template <typename TT>
auto btree<TT>::next_leaf(key_type const &k) const -> leaf_type *
{
	if (this->root_ == nullptr) [[unlikely]] {
		return nullptr;
	}

	/*
	 * Descend to k, remembering the deepest node that still had a child
	 * to the right. That child's leftmost leaf is the next one.
	 */
	node_type *candidate = nullptr;
	node_type *node = this->root_;
	while (!node->is_leaf) {
		auto interior = static_cast<interior_type *>(node);
		auto const i = interior->get_node_id_containing_key(k);
		if (i + 1 < interior->get_node_n()) {
			candidate = interior->tree_nodes[i + 1];
		}
		node = interior->tree_nodes[i];
	}

	if (candidate == nullptr) {
		return nullptr;
	}
	while (!candidate->is_leaf) {
		candidate = static_cast<interior_type *>(candidate)->tree_nodes[0];
	}
	auto leaf = static_cast<leaf_type *>(candidate);
	ensure_leaf_sorted(leaf);
	return leaf;
}

template <typename TT>
auto btree<TT>::prev_leaf(key_type const &k) const -> leaf_type *
{
	if (this->root_ == nullptr) [[unlikely]] {
		return nullptr;
	}

	node_type *candidate = nullptr;
	node_type *node = this->root_;
	while (!node->is_leaf) {
		auto interior = static_cast<interior_type *>(node);
		auto const i = interior->get_node_id_containing_key(k);
		if (i > 0) {
			candidate = interior->tree_nodes[i - 1];
		}
		node = interior->tree_nodes[i];
	}

	if (candidate == nullptr) {
		return nullptr;
	}
	while (!candidate->is_leaf) {
		auto interior = static_cast<interior_type *>(candidate);
		candidate = interior->tree_nodes[interior->get_node_n() - 1];
	}
	auto leaf = static_cast<leaf_type *>(candidate);
	ensure_leaf_sorted(leaf);
	return leaf;
}

template <typename TT>
auto btree<TT>::const_iterator::operator++() -> const_iterator &
{
	assert(this->leaf_ != nullptr);

	++this->idx_;
	if (this->idx_ < static_cast<buf_size_type>(this->leaf_->size)) {
		return *this;
	}

	auto const last =
		entry_traits::get_key(this->leaf_->elements[this->leaf_->size - 1]);
	this->leaf_ = this->tree_->next_leaf(last);
	this->idx_ = 0;
	return *this;
}

template <typename TT>
auto btree<TT>::const_iterator::operator--() -> const_iterator &
{
	if (this->leaf_ == nullptr) {
		/* Stepping back from end(). */
		this->leaf_ = this->tree_->rightmost_leaf();
		assert(this->leaf_ != nullptr);
		this->idx_ = static_cast<buf_size_type>(this->leaf_->size) - 1;
		return *this;
	}

	if (this->idx_ > 0) {
		--this->idx_;
		return *this;
	}

	auto const first = entry_traits::get_key(this->leaf_->elements[0]);
	this->leaf_ = this->tree_->prev_leaf(first);
	assert(this->leaf_ != nullptr);
	this->idx_ = static_cast<buf_size_type>(this->leaf_->size) - 1;
	return *this;
}

template <typename TT>
auto btree<TT>::begin() const -> const_iterator
{
	leaf_type *leaf = this->leftmost_leaf();
	/* An empty tree, or a root leaf holding nothing, is already end(). */
	if (leaf == nullptr || leaf->size == 0) {
		return this->end();
	}
	return const_iterator(this, leaf, 0);
}

template <typename TT>
auto btree<TT>::lower_bound(key_type const &k) const -> const_iterator
{
	leaf_type *leaf = this->leaf_covering(k);
	if (leaf == nullptr) {
		return this->end();
	}

	auto const idx = static_cast<buf_size_type>(leaf->binary_search_key(k));
	if (idx < static_cast<buf_size_type>(leaf->size)) {
		return const_iterator(this, leaf, idx);
	}

	/* Every key here is below k; the answer opens the next leaf. */
	if (leaf->size == 0) [[unlikely]] {
		return this->end();
	}
	leaf_type *next =
		this->next_leaf(entry_traits::get_key(leaf->elements[leaf->size - 1]));
	return next == nullptr ? this->end() : const_iterator(this, next, 0);
}

template <typename TT>
auto btree<TT>::upper_bound(key_type const &k) const -> const_iterator
{
	leaf_type *leaf = this->leaf_covering(k);
	if (leaf == nullptr) {
		return this->end();
	}

	auto const idx =
		static_cast<buf_size_type>(leaf->binary_search_key_exclusive(k));
	if (idx < static_cast<buf_size_type>(leaf->size)) {
		return const_iterator(this, leaf, idx);
	}

	if (leaf->size == 0) [[unlikely]] {
		return this->end();
	}
	leaf_type *next =
		this->next_leaf(entry_traits::get_key(leaf->elements[leaf->size - 1]));
	return next == nullptr ? this->end() : const_iterator(this, next, 0);
}

template <typename TT>
auto btree<TT>::find(key_type const &k) const -> const_iterator
{
	auto const it = this->lower_bound(k);
	if (it == this->end() || !TT::key_equal(entry_traits::get_key(*it), k)) {
		return this->end();
	}
	return it;
}

template <typename TT>
auto btree<TT>::equal_range(key_type const &k) const
	-> std::pair<const_iterator, const_iterator>
{
	auto const lo = this->lower_bound(k);
	if (lo == this->end() || !TT::key_equal(entry_traits::get_key(*lo), k)) {
		return { lo, lo };
	}
	auto hi = lo;
	++hi;
	return { lo, hi };
}

template <typename TT>
void btree<TT>::erase(const_iterator pos)
{
	assert(pos != this->end());
	this->batch_delete(std::array<key_type, 1>{ entry_traits::get_key(*pos) });
}

template <typename TT>
void btree<TT>::erase(const_iterator first, const_iterator last)
{
	typename TT::key_seq keys;
	for (auto it = first; it != last; ++it) {
		keys.push_back(entry_traits::get_key(*it));
	}
	if (keys.empty()) {
		return;
	}
	/* The keys came out in order. */
	this->batch_delete(keys, TT::leaf_entries_pattern::hybrid, true,
					   default_hybrid_pred{});
}

} /* namespace pab_tree */

#endif /* PABTREE_B_TREE_IMPL_QUERY_OP_ITERATOR_HPP_ */

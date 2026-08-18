#ifndef PABTREE_B_TREE_IMPL_QUERY_OP_ITERATOR_HPP_
#define PABTREE_B_TREE_IMPL_QUERY_OP_ITERATOR_HPP_

#include <algorithm>
#include <cassert>
#include <utility>

#include <parlay/parallel.h>

#include "pab_tree/b_tree.h"

namespace pab_tree
{

template <typename type_trait>
void btree<type_trait>::ensure_leaf_sorted(leaf_type *TL)
{
	if (!TL->is_sorted) {
		std::sort(TL->elements.begin(), TL->elements.begin() + TL->size,
				  key_less);
		TL->is_sorted = true;
	}
}

template <typename type_trait>
void btree<type_trait>::sort_leaves_rec(node_type *T)
{
	if (T->is_leaf) {
		ensure_leaf_sorted(static_cast<leaf_type *>(T));
		return;
	}

	auto TI = static_cast<interior_type *>(T);
	auto const node_n = TI->get_node_n();
	parlay::parallel_for(
		0, node_n, [&](buf_size_type i) { sort_leaves_rec(TI->tree_nodes[i]); },
		1);
}

template <typename type_trait> void btree<type_trait>::sort_leaves() const
{
	if (this->root_ == nullptr) [[unlikely]] {
		return;
	}
	sort_leaves_rec(this->root_);
}

template <typename type_trait>
auto btree<type_trait>::leftmost_leaf() const -> leaf_type *
{
	if (this->root_ == nullptr) [[unlikely]] {
		return nullptr;
	}

	node_type *T = this->root_;
	while (!T->is_leaf) {
		T = static_cast<interior_type *>(T)->tree_nodes[0];
	}
	auto TL = static_cast<leaf_type *>(T);
	ensure_leaf_sorted(TL);
	return TL;
}

template <typename type_trait>
auto btree<type_trait>::rightmost_leaf() const -> leaf_type *
{
	if (this->root_ == nullptr) [[unlikely]] {
		return nullptr;
	}

	node_type *T = this->root_;
	while (!T->is_leaf) {
		auto TI = static_cast<interior_type *>(T);
		T = TI->tree_nodes[TI->get_node_n() - 1];
	}
	auto TL = static_cast<leaf_type *>(T);
	ensure_leaf_sorted(TL);
	return TL;
}

template <typename type_trait>
auto btree<type_trait>::leaf_covering(key_type const &k) const -> leaf_type *
{
	if (this->root_ == nullptr) [[unlikely]] {
		return nullptr;
	}

	node_type *T = this->root_;
	while (!T->is_leaf) {
		auto TI = static_cast<interior_type *>(T);
		T = TI->tree_nodes[TI->get_node_id_containing_key(k)];
	}
	auto TL = static_cast<leaf_type *>(T);
	ensure_leaf_sorted(TL);
	return TL;
}

template <typename type_trait>
auto btree<type_trait>::next_leaf(key_type const &k) const -> leaf_type *
{
	if (this->root_ == nullptr) [[unlikely]] {
		return nullptr;
	}

	// Descend to k, remembering the deepest node that still had a child
	// to the right. That child's leftmost leaf is the next one.
	node_type *candidate = nullptr;
	node_type *T = this->root_;
	while (!T->is_leaf) {
		auto TI = static_cast<interior_type *>(T);
		auto const i = TI->get_node_id_containing_key(k);
		if (i + 1 < TI->get_node_n()) {
			candidate = TI->tree_nodes[i + 1];
		}
		T = TI->tree_nodes[i];
	}

	if (candidate == nullptr) {
		return nullptr;
	}
	while (!candidate->is_leaf) {
		candidate = static_cast<interior_type *>(candidate)->tree_nodes[0];
	}
	auto TL = static_cast<leaf_type *>(candidate);
	ensure_leaf_sorted(TL);
	return TL;
}

template <typename type_trait>
auto btree<type_trait>::prev_leaf(key_type const &k) const -> leaf_type *
{
	if (this->root_ == nullptr) [[unlikely]] {
		return nullptr;
	}

	node_type *candidate = nullptr;
	node_type *T = this->root_;
	while (!T->is_leaf) {
		auto TI = static_cast<interior_type *>(T);
		auto const i = TI->get_node_id_containing_key(k);
		if (i > 0) {
			candidate = TI->tree_nodes[i - 1];
		}
		T = TI->tree_nodes[i];
	}

	if (candidate == nullptr) {
		return nullptr;
	}
	while (!candidate->is_leaf) {
		auto TI = static_cast<interior_type *>(candidate);
		candidate = TI->tree_nodes[TI->get_node_n() - 1];
	}
	auto TL = static_cast<leaf_type *>(candidate);
	ensure_leaf_sorted(TL);
	return TL;
}

template <typename type_trait>
auto btree<type_trait>::const_iterator::operator++() -> const_iterator &
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

template <typename type_trait>
auto btree<type_trait>::const_iterator::operator--() -> const_iterator &
{
	if (this->leaf_ == nullptr) {
		// Stepping back from end().
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

template <typename type_trait>
auto btree<type_trait>::begin() const -> const_iterator
{
	leaf_type *leaf = this->leftmost_leaf();
	// An empty tree, or a root leaf holding nothing, is already end().
	if (leaf == nullptr || leaf->size == 0) {
		return this->end();
	}
	return const_iterator(this, leaf, 0);
}

template <typename type_trait>
auto btree<type_trait>::lower_bound(key_type const &k) const -> const_iterator
{
	leaf_type *leaf = this->leaf_covering(k);
	if (leaf == nullptr) {
		return this->end();
	}

	auto const idx = static_cast<buf_size_type>(leaf->binary_search_key(k));
	if (idx < static_cast<buf_size_type>(leaf->size)) {
		return const_iterator(this, leaf, idx);
	}

	// Every key here is below k; the answer opens the next leaf.
	if (leaf->size == 0) [[unlikely]] {
		return this->end();
	}
	leaf_type *next =
		this->next_leaf(entry_traits::get_key(leaf->elements[leaf->size - 1]));
	return next == nullptr ? this->end() : const_iterator(this, next, 0);
}

template <typename type_trait>
auto btree<type_trait>::upper_bound(key_type const &k) const -> const_iterator
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

template <typename type_trait>
auto btree<type_trait>::find(key_type const &k) const -> const_iterator
{
	auto const it = this->lower_bound(k);
	if (it == this->end() ||
		!type_trait::key_equal(entry_traits::get_key(*it), k)) {
		return this->end();
	}
	return it;
}

template <typename type_trait>
auto btree<type_trait>::equal_range(key_type const &k) const
	-> std::pair<const_iterator, const_iterator>
{
	auto const lo = this->lower_bound(k);
	if (lo == this->end() ||
		!type_trait::key_equal(entry_traits::get_key(*lo), k)) {
		return { lo, lo };
	}
	auto hi = lo;
	++hi;
	return { lo, hi };
}

template <typename type_trait> void btree<type_trait>::erase(const_iterator pos)
{
	assert(pos != this->end());
	this->batch_delete(std::array<key_type, 1>{ entry_traits::get_key(*pos) });
}

template <typename type_trait>
void btree<type_trait>::erase(const_iterator first, const_iterator last)
{
	typename type_trait::key_seq keys;
	for (auto it = first; it != last; ++it) {
		keys.push_back(entry_traits::get_key(*it));
	}
	if (keys.empty()) {
		return;
	}
	// The keys came out in order.
	this->batch_delete(keys, type_trait::leaf_entries_pattern::hybrid, true,
					   default_hybrid_pred{});
}

} // namespace pab_tree

#endif // PABTREE_B_TREE_IMPL_QUERY_OP_ITERATOR_HPP_

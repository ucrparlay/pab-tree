#ifndef PABTREE_B_TREE_IMPL_QUERY_OP_ITERATOR_HPP_
#define PABTREE_B_TREE_IMPL_QUERY_OP_ITERATOR_HPP_

#include <algorithm>
#include <cassert>
#include <utility>

#include <parlay/parallel.h>

#include "pab_tree/b_tree.h"

namespace pab_tree
{

template <typename TT> void btree<TT>::ensure_leaf_sorted(Leaf *TL)
{
	if (!TL->is_sorted) {
		std::sort(TL->elements.begin(), TL->elements.begin() + TL->size,
				  key_less);
		TL->is_sorted = true;
	}
}

template <typename TT> void btree<TT>::sort_leaves_rec(Node *T)
{
	if (T->is_leaf) {
		ensure_leaf_sorted(static_cast<Leaf *>(T));
		return;
	}

	auto TI = static_cast<Interior *>(T);
	auto const node_n = TI->get_node_n();
	parlay::parallel_for(
		0, node_n, [&](buf_size_type i) { sort_leaves_rec(TI->tree_nodes[i]); },
		1);
}

template <typename TT> void btree<TT>::sort_leaves() const
{
	if (this->root_ == nullptr) [[unlikely]] {
		return;
	}
	sort_leaves_rec(this->root_);
}

template <typename TT> auto btree<TT>::leftmost_leaf() const -> Leaf *
{
	if (this->root_ == nullptr) [[unlikely]] {
		return nullptr;
	}

	Node *T = this->root_;
	while (!T->is_leaf) {
		T = static_cast<Interior *>(T)->tree_nodes[0];
	}
	auto TL = static_cast<Leaf *>(T);
	ensure_leaf_sorted(TL);
	return TL;
}

template <typename TT> auto btree<TT>::rightmost_leaf() const -> Leaf *
{
	if (this->root_ == nullptr) [[unlikely]] {
		return nullptr;
	}

	Node *T = this->root_;
	while (!T->is_leaf) {
		auto TI = static_cast<Interior *>(T);
		T = TI->tree_nodes[TI->get_node_n() - 1];
	}
	auto TL = static_cast<Leaf *>(T);
	ensure_leaf_sorted(TL);
	return TL;
}

template <typename TT> auto btree<TT>::leaf_covering(K const &k) const -> Leaf *
{
	if (this->root_ == nullptr) [[unlikely]] {
		return nullptr;
	}

	Node *T = this->root_;
	while (!T->is_leaf) {
		auto TI = static_cast<Interior *>(T);
		T = TI->tree_nodes[TI->get_node_id_containing_key(k)];
	}
	auto TL = static_cast<Leaf *>(T);
	ensure_leaf_sorted(TL);
	return TL;
}

template <typename TT> auto btree<TT>::next_leaf(K const &k) const -> Leaf *
{
	if (this->root_ == nullptr) [[unlikely]] {
		return nullptr;
	}

	// Descend to k, remembering the deepest node that still had a child
	// to the right. That child's leftmost leaf is the next one.
	Node *candidate = nullptr;
	Node *T = this->root_;
	while (!T->is_leaf) {
		auto TI = static_cast<Interior *>(T);
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
		candidate = static_cast<Interior *>(candidate)->tree_nodes[0];
	}
	auto TL = static_cast<Leaf *>(candidate);
	ensure_leaf_sorted(TL);
	return TL;
}

template <typename TT> auto btree<TT>::prev_leaf(K const &k) const -> Leaf *
{
	if (this->root_ == nullptr) [[unlikely]] {
		return nullptr;
	}

	Node *candidate = nullptr;
	Node *T = this->root_;
	while (!T->is_leaf) {
		auto TI = static_cast<Interior *>(T);
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
		auto TI = static_cast<Interior *>(candidate);
		candidate = TI->tree_nodes[TI->get_node_n() - 1];
	}
	auto TL = static_cast<Leaf *>(candidate);
	ensure_leaf_sorted(TL);
	return TL;
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
		Entry::get_key(this->leaf_->elements[this->leaf_->size - 1]);
	this->leaf_ = this->tree_->next_leaf(last);
	this->idx_ = 0;
	return *this;
}

template <typename TT>
auto btree<TT>::const_iterator::operator--() -> const_iterator &
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

	auto const first = Entry::get_key(this->leaf_->elements[0]);
	this->leaf_ = this->tree_->prev_leaf(first);
	assert(this->leaf_ != nullptr);
	this->idx_ = static_cast<buf_size_type>(this->leaf_->size) - 1;
	return *this;
}

template <typename TT> auto btree<TT>::begin() const -> const_iterator
{
	Leaf *leaf = this->leftmost_leaf();
	// An empty tree, or a root leaf holding nothing, is already end().
	if (leaf == nullptr || leaf->size == 0) {
		return this->end();
	}
	return const_iterator(this, leaf, 0);
}

template <typename TT>
auto btree<TT>::lower_bound(K const &k) const -> const_iterator
{
	Leaf *leaf = this->leaf_covering(k);
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
	Leaf *next =
		this->next_leaf(Entry::get_key(leaf->elements[leaf->size - 1]));
	return next == nullptr ? this->end() : const_iterator(this, next, 0);
}

template <typename TT>
auto btree<TT>::upper_bound(K const &k) const -> const_iterator
{
	Leaf *leaf = this->leaf_covering(k);
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
	Leaf *next =
		this->next_leaf(Entry::get_key(leaf->elements[leaf->size - 1]));
	return next == nullptr ? this->end() : const_iterator(this, next, 0);
}

template <typename TT> auto btree<TT>::find(K const &k) const -> const_iterator
{
	auto const it = this->lower_bound(k);
	if (it == this->end() || !TT::key_equal(Entry::get_key(*it), k)) {
		return this->end();
	}
	return it;
}

template <typename TT>
auto btree<TT>::equal_range(K const &k) const
	-> std::pair<const_iterator, const_iterator>
{
	auto const lo = this->lower_bound(k);
	if (lo == this->end() || !TT::key_equal(Entry::get_key(*lo), k)) {
		return { lo, lo };
	}
	auto hi = lo;
	++hi;
	return { lo, hi };
}

template <typename TT> void btree<TT>::erase(const_iterator pos)
{
	assert(pos != this->end());
	this->batch_delete(std::array<K, 1>{ Entry::get_key(*pos) });
}

template <typename TT>
void btree<TT>::erase(const_iterator first, const_iterator last)
{
	typename TT::key_seq keys;
	for (auto it = first; it != last; ++it) {
		keys.push_back(Entry::get_key(*it));
	}
	if (keys.empty()) {
		return;
	}
	// The keys came out in order.
	this->batch_delete(keys, TT::leaf_entries_pattern::hybrid, true,
					   default_hybrid_pred{});
}

} // namespace pab_tree

#endif // PABTREE_B_TREE_IMPL_QUERY_OP_ITERATOR_HPP_

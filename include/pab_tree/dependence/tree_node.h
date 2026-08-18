#ifndef PABTREE_DEPENDENCE_TREE_NODE_H_
#define PABTREE_DEPENDENCE_TREE_NODE_H_

#include <cassert>

#include <parlay/primitives.h>
#include <parlay/slice.h>

#include <algorithm>
#include <array>
#include <bitset>
#include <concepts>
#include <cstdint>
#include <new>
#include <numeric>
#include <tuple>
#include <type_traits>
#include <variant>

#include "pab_tree/dependence/comparator.h"
#include "pab_tree/dependence/node_base.h"
#include "pab_tree/dependence/concepts.h"
#include "pab_tree/dependence/binary_search.h"
#include "pab_tree/dependence/box_op.h"
#include "parlay/utilities.h"

namespace pab_tree
{

struct alloc_normal_leaf_tag {};
struct alloc_empty_leaf_tag {};

template <typename TypeTrait, typename EleAssignTag = parlay::copy_assign_tag>
struct leaf_node : TypeTrait::Node {
	using TT = TypeTrait;
	using Node = TT::Node;
	using E = typename TT::E;
	using K = typename TT::K;
	using Entry = TT::Entry;
	using entry_seq = typename TT::entry_seq;
	using entry_arr = typename TT::entry_arr;
	using AT = typename TT::leaf_aug;
	using ele_assign_tag_type = EleAssignTag;

	static constexpr typename TT::buf_size_type k_binary_search_base =
		static_cast<typename TT::buf_size_type>(
#ifdef __cpp_lib_hardware_interference_size
			std::hardware_constructive_interference_size / sizeof(E)
#else
			64 / sizeof(E)
#endif
		);

	// default allocator
	leaf_node()
		: Node{ true, static_cast<size_t>(0) }
		, aug(AT())
		, is_sorted{ true }
	{
	}

	leaf_node(AT const &_aug)
		: Node{ true, static_cast<size_t>(0) }
		, aug(_aug)
		, is_sorted{ true }
	{
	}

	// alloc a leaf with default size
	template <typename Range>
	leaf_node(Range In, alloc_normal_leaf_tag)
		: Node{ true, static_cast<size_t>(In.size()) }
		, aug(AT(In))
		, is_sorted{ true }
	{
		assert(In.size() <= TT::k_buf_ub);
		assert(parlay::is_sorted(
			In, [](auto a, auto b) { return TT::entry_less(a, b); }));
		// elements is left default-initialised: copy_n fills [0, size)
		// and nothing reads past it. Value-initialising the whole
		// k_buf_ub array here cost more than the copy.
		std::ranges::copy_n(In.begin(), In.size(), elements.begin());
	}

	[[nodiscard]] bool capacity_full() const
	{
		return this->size >= TT::k_buf_ub;
	}

	// return the first idx s.t. Entry::get_key(elements[idx]) >= k
	auto binary_search_key(K const &k) const
	{
		assert(this->is_sorted);
		return pab_tree::detail::binary_search<k_binary_search_base>(
			parlay::make_slice(this->elements.begin(),
							   this->elements.begin() + this->size),
			[k](E const e) { return TT::key_less(Entry::get_key(e), k); });
	}

	// return the first idx s.t. Entry::get_key(elements[idx]) > k
	auto binary_search_key_exclusive(K const k) const
	{
		assert(this->is_sorted);
		return pab_tree::detail::binary_search<k_binary_search_base>(
			parlay::make_slice(this->elements.begin(),
							   this->elements.begin() + this->size),
			[k](E const e) { return !TT::key_less(k, Entry::get_key(e)); });
	}

	bool contain(K const &k) const
	{
		return this->is_sorted ?
				   ([&] {
					   auto idx = this->binary_search_key(k);
					   return idx < this->size &&
							  TT::key_equal(Entry::get_key(this->elements[idx]),
											k);
				   })() :
				   (std::ranges::find_if(
						this->elements.begin(),
						this->elements.begin() + this->size, [&](E const &e) {
							return TT::key_equal(Entry::get_key(e), k);
						}) != this->elements.begin() + this->size);
	}

	entry_seq get_ele() const
	{
		return entry_seq(this->elements.begin(),
						 this->elements.begin() + this->size);
	}

	auto get_ele_begin_iter()
	{
		return this->elements.begin();
	}

	auto get_ele_rbegin_iter()
	{
		return this->elements.begin() + this->size - 1;
	}

	auto get_ele_end_iter()
	{
		return this->elements.begin() + this->size;
	}

	auto get_ele_slice() const
	{
		return parlay::make_slice(this->elements.begin(),
								  this->elements.begin() + this->size);
	}

	auto const &get_ele_container() const
	{
		return this->elements;
	}

	entry_arr &get_ele_container()
	{
		return this->elements;
	}

	void initialize_ele_seq()
	{
		this->elements = entry_arr();
	}

	AT const &get_aug() const
	{
		return aug;
	}

	AT &get_aug()
	{
		return aug;
	}

	// TODO: should put this one in the aug as well and don't use the
	// monostate
	template <typename Range> void update_aug(Range In)
	{
		if constexpr (node_has_non_trivial_aug<AT>) {
			aug.update_aug(In);
		}
	}

	auto reset_aug()
	{
		return aug.reset();
	}

	entry_arr elements;
	AT aug;
	bool is_sorted;
};

// Alloc a leaf
template <typename Range, typename Leaf>
static Leaf *alloc_normal_leaf_node(Range In)
{
	Leaf *o = parlay::type_allocator<Leaf>::alloc();
	new (o) Leaf(In, alloc_normal_leaf_tag());
	return o;
}

// Alloc a empty Leaf
template <typename Range, typename Leaf> static Leaf *alloc_empty_leaf_node()
{
	Leaf *o = parlay::type_allocator<Leaf>::alloc();
	new (o) Leaf();
	return o;
}

// Alloc a empty Leaf, but set the aug by hand
template <typename Range, typename Leaf>
static Leaf *alloc_empty_leaf_node(typename Leaf::AT const &aug)
{
	Leaf *o = parlay::type_allocator<Leaf>::alloc();
	new (o) Leaf(aug);
	return o;
}

// multi-way node whose children # is fixed
template <typename TypeTrait> struct multi_way_node : TypeTrait::Node {
	using TT = TypeTrait;
	using Node = TT::Node;
	using buf_size_type = typename TT::buf_size_type;

	static consteval auto get_levels()
	{
		buf_size_type a = 0;
		auto temp = TT::k_buf_ub;
		while (temp > 1) {
			temp >>= 1; // divide by 2
			a++;
		}
		return a;
	}

	using node_arr = std::array<Node *, TT::k_buf_ub>;
	using split_arr = std::array<typename TT::Split, TT::k_buf_ub>;

	static constexpr buf_size_type k_binary_search_base =
		static_cast<buf_size_type>(
#ifdef __cpp_lib_hardware_interference_size
			std::hardware_constructive_interference_size /
			sizeof(typename TT::K)
#else
			64 / sizeof(typename TT::K)
#endif
		);

	multi_way_node(TT::node_seq const &_tree_nodes, TT::split_seq const &_split,
				   TT::interior_aug const &_aug)
		: Node{ false, std::accumulate(_tree_nodes.begin(), _tree_nodes.end(),
									   static_cast<size_t>(0),
									   [](size_t acc, Node *n) -> size_t {
										   return acc + n->size;
									   }) }
		, node_n(_tree_nodes.size())
		, aug(_aug)
	{
		for (buf_size_type i = 0; i < _tree_nodes.size(); i++) {
			tree_nodes[i] = _tree_nodes[i];
		}
		for (buf_size_type i = 0; i < _split.size(); i++) {
			split[i] = _split[i];
		}
		assert(this->get_node_n() == _tree_nodes.size());
	}

	multi_way_node(TT::node_slice _tree_nodes_slice,
				   TT::split_slice _split_slice, TT::interior_aug const &_aug)
		: Node{ false,
				std::accumulate(_tree_nodes_slice.begin(),
								_tree_nodes_slice.end(), static_cast<size_t>(0),
								[](size_t acc, Node *n) -> size_t {
									return acc + n->size;
								}) }
		, node_n(_tree_nodes_slice.size())
		, aug(_aug)
	{
		for (buf_size_type i = 0; i < _tree_nodes_slice.size(); i++) {
			tree_nodes[i] = _tree_nodes_slice[i];
		}
		for (buf_size_type i = 0; i < _split_slice.size(); i++) {
			split[i] = _split_slice[i];
		}
	}

	TT::buf_size_type get_node_n() const
	{
		return node_n;
	}

	TT::buf_size_type get_split_n() const
	{
		return node_n == 0 ? 0 : node_n - 1;
	}

	// Same accessors leaf_node exposes.
	TT::interior_aug const &get_aug() const
	{
		return aug;
	}

	TT::interior_aug &get_aug()
	{
		return aug;
	}

	split_arr const &get_split() const
	{
		return split;
	}

	auto get_node_slice() const
	{
		return parlay::make_slice(tree_nodes.begin(),
								  tree_nodes.begin() + get_node_n());
	}

	auto get_split_slice() const
	{
		return parlay::make_slice(split.begin(), split.begin() + get_split_n());
	}

	TT::buf_size_type get_node_id_containing_key(TT::K const k) const
	{
		// return the first idx s.t. split[idx] > k
		// i.e.: if split[1] == k, then k belongs to tree_nodes[2]
		return pab_tree::detail::binary_search<k_binary_search_base>(
			this->get_split_slice(),
			[&](TT::K const key) { return !TT::key_less(k, key); });
	}

	TT::buf_size_type get_node_id_containing_key_exclusive(TT::K const k) const
	{
		// return the first idx s.t. split[idx] >= k
		// i.e.: if split[1] == k, then k belongs to tree_nodes[1]
		return pab_tree::detail::binary_search<k_binary_search_base>(
			this->get_split_slice(),
			[&](TT::K const key) { return TT::key_less(key, k); });
	}

	bool validate_key_in_node(TT::buf_size_type node_idx, TT::K const k) const
	{
		if (this->get_split_n() == 0) {
			return true; // one child: every key belongs to it
		}
		// a >= b is !(a < b); a < b is key_less directly.
		return node_idx == 0 ?
				   TT::key_less(k, this->split.front()) :
				   (node_idx == this->get_split_n() ?
						!TT::key_less(k, this->split[this->get_split_n() - 1]) :
						(!TT::key_less(k, this->split[node_idx - 1]) &&
						 TT::key_less(k, this->split[node_idx])));
	}

	TT::buf_size_type node_n;
	node_arr tree_nodes;
	split_arr split;
	TT::interior_aug aug;
};

template <typename Interior, typename TT>
static Interior *alloc_interior_node(typename TT::node_seq const &tree_nodes,
									 typename TT::split_seq const &split)
{
	Interior *o = parlay::type_allocator<Interior>::alloc();
	new (o) Interior(tree_nodes, split);
	return o;
}

template <typename Interior, typename TT>
static Interior *alloc_interior_node(typename TT::node_slice tree_nodes_slice,
									 typename TT::split_slice split_slice)
{
	Interior *o = parlay::type_allocator<Interior>::alloc();
	new (o) Interior(tree_nodes_slice, split_slice);
	return o;
}

// TODO: maybe replace the node_arr, ST, and AT with template parameters
template <typename Interior, typename TT>
static Interior *alloc_interior_node(typename TT::node_slice const &tree_nodes,
									 typename TT::split_seq const &split,
									 typename TT::interior_aug const &aug)
{
	Interior *o = parlay::type_allocator<Interior>::alloc();
	new (o) Interior(tree_nodes, split, aug);
	return o;
}

template <typename Interior, typename TT> static Interior *alloc_interior_node()
{
	Interior *o = parlay::type_allocator<Interior>::alloc();
	new (o) Interior();
	return o;
}

template <typename NodeType> static void free_node(auto T)
{
	parlay::type_allocator<NodeType>::retire(static_cast<NodeType *>(T));
}

} // namespace pab_tree

#endif // PABTREE_DEPENDENCE_TREE_NODE_H_

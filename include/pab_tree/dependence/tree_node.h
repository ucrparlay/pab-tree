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

template <typename TT>
struct leaf_node : TT::node_type {
	using type_trait = TT;
	using node_type = type_trait::node_type;
	using entry_type = typename type_trait::entry_type;
	using key_type = typename type_trait::key_type;
	using entry_traits = type_trait::entry_traits;
	using entry_seq = typename type_trait::entry_seq;
	using entry_arr = typename type_trait::entry_arr;
	using aug_type = typename type_trait::leaf_aug;

	static constexpr typename type_trait::buf_size_type k_binary_search_base =
		static_cast<typename type_trait::buf_size_type>(
#ifdef __cpp_lib_hardware_interference_size
			std::hardware_constructive_interference_size / sizeof(entry_type)
#else
			64 / sizeof(entry_type)
#endif
		);

	/* default allocator */
	leaf_node()
		: node_type{ true, static_cast<size_t>(0) }
		, aug(aug_type())
		, is_sorted{ true }
	{
	}

	leaf_node(aug_type const &_aug)
		: node_type{ true, static_cast<size_t>(0) }
		, aug(_aug)
		, is_sorted{ true }
	{
	}

	/* alloc a leaf with default size */
	template <typename R>
	leaf_node(R in, alloc_normal_leaf_tag)
		: node_type{ true, static_cast<size_t>(in.size()) }
		, aug(aug_type(in))
		, is_sorted{ true }
	{
		static_assert(!node_has_non_trivial_aug<aug_type> ||
						  leaf_aug_constructible_from<aug_type, R>,
					  "leaf augmentation needs a constructor taking the "
					  "leaf's entries");
		assert(in.size() <= type_trait::k_buf_ub);
		assert(parlay::is_sorted(
			in, [](auto a, auto b) { return type_trait::entry_less(a, b); }));
		/*
		 * elements is left default-initialised: copy_n fills [0, size)
		 * and nothing reads past it. Value-initialising the whole
		 * k_buf_ub array here cost more than the copy.
		 */
		std::ranges::copy_n(in.begin(), in.size(), elements.begin());
	}

	[[nodiscard]] bool capacity_full() const
	{
		return this->size >= type_trait::k_buf_ub;
	}

	/* return the first idx s.t. entry_traits::get_key(elements[idx]) >= k */
	auto binary_search_key(key_type const &k) const
	{
		assert(this->is_sorted);
		return pab_tree::detail::binary_search<k_binary_search_base>(
			parlay::make_slice(this->elements.begin(),
							   this->elements.begin() + this->size),
			[k](entry_type const e) {
				return type_trait::key_less(entry_traits::get_key(e), k);
			});
	}

	/* return the first idx s.t. entry_traits::get_key(elements[idx]) > k */
	auto binary_search_key_exclusive(key_type const k) const
	{
		assert(this->is_sorted);
		return pab_tree::detail::binary_search<k_binary_search_base>(
			parlay::make_slice(this->elements.begin(),
							   this->elements.begin() + this->size),
			[k](entry_type const e) {
				return !type_trait::key_less(k, entry_traits::get_key(e));
			});
	}

	bool contain(key_type const &k) const
	{
		return this->is_sorted ?
				   ([&] {
					   auto idx = this->binary_search_key(k);
					   return idx < this->size &&
							  type_trait::key_equal(
								  entry_traits::get_key(this->elements[idx]),
								  k);
				   })() :
				   (std::ranges::find_if(this->elements.begin(),
										 this->elements.begin() + this->size,
										 [&](entry_type const &e) {
											 return type_trait::key_equal(
												 entry_traits::get_key(e), k);
										 }) !=
					this->elements.begin() + this->size);
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

	aug_type const &get_aug() const
	{
		return aug;
	}

	aug_type &get_aug()
	{
		return aug;
	}

	/*
	 * TODO: should put this one in the aug as well and don't use the
	 * monostate
	 */
	template <typename R>
	void update_aug(R in)
	{
		if constexpr (node_has_non_trivial_aug<aug_type>) {
			static_assert(leaf_aug_updatable_with<aug_type, R>,
						  "leaf augmentation needs "
						  "update_aug(range-of-entries)");
			aug.update_aug(in);
		}
	}

	auto reset_aug()
	{
		return aug.reset();
	}

	entry_arr elements;
	aug_type aug;
	bool is_sorted;
};

/* Alloc a leaf */
template <typename R, typename LeafType>
static LeafType *alloc_normal_leaf_node(R in)
{
	LeafType *o = LeafType::type_trait::node_alloc::template alloc<LeafType>();
	new (o) LeafType(in, alloc_normal_leaf_tag());
	return o;
}

/* Alloc an empty leaf */
template <typename LeafType>
static LeafType *alloc_empty_leaf_node()
{
	LeafType *o = LeafType::type_trait::node_alloc::template alloc<LeafType>();
	new (o) LeafType();
	return o;
}

/* Alloc an empty leaf, but set the aug by hand */
template <typename LeafType>
static LeafType *alloc_empty_leaf_node(typename LeafType::aug_type const &aug)
{
	LeafType *o = LeafType::type_trait::node_alloc::template alloc<LeafType>();
	new (o) LeafType(aug);
	return o;
}

/* multi-way node whose children # is fixed */
template <typename TT>
struct multi_way_node : TT::node_type {
	using type_trait = TT;
	using node_type = type_trait::node_type;
	using buf_size_type = typename type_trait::buf_size_type;

	static consteval auto get_levels()
	{
		buf_size_type a = 0;
		auto temp = type_trait::k_buf_ub;
		while (temp > 1) {
			temp >>= 1; /* divide by 2 */
			a++;
		}
		return a;
	}

	using node_arr = std::array<node_type *, type_trait::k_buf_ub>;
	using split_arr =
		std::array<typename type_trait::split_type, type_trait::k_buf_ub>;

	static constexpr buf_size_type k_binary_search_base =
		static_cast<buf_size_type>(
#ifdef __cpp_lib_hardware_interference_size
			std::hardware_constructive_interference_size /
			sizeof(typename type_trait::key_type)
#else
			64 / sizeof(typename type_trait::key_type)
#endif
		);

	multi_way_node(type_trait::node_seq const &_tree_nodes,
				   type_trait::split_seq const &_split,
				   type_trait::interior_aug const &_aug)
		: node_type{ false,
					 std::accumulate(_tree_nodes.begin(), _tree_nodes.end(),
									 static_cast<size_t>(0),
									 [](size_t acc, node_type *n) -> size_t {
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

	multi_way_node(type_trait::node_slice _tree_nodes_slice,
				   type_trait::split_slice _split_slice,
				   type_trait::interior_aug const &_aug)
		: node_type{ false, std::accumulate(
								_tree_nodes_slice.begin(),
								_tree_nodes_slice.end(), static_cast<size_t>(0),
								[](size_t acc, node_type *n) -> size_t {
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

	type_trait::buf_size_type get_node_n() const
	{
		return node_n;
	}

	type_trait::buf_size_type get_split_n() const
	{
		return node_n == 0 ? 0 : node_n - 1;
	}

	/* Same accessors leaf_node exposes. */
	type_trait::interior_aug const &get_aug() const
	{
		return aug;
	}

	type_trait::interior_aug &get_aug()
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

	type_trait::buf_size_type
	get_node_id_containing_key(type_trait::key_type const k) const
	{
		/*
		 * return the first idx s.t. split[idx] > k
		 * i.e.: if split[1] == k, then k belongs to tree_nodes[2]
		 */
		return pab_tree::detail::binary_search<k_binary_search_base>(
			this->get_split_slice(), [&](type_trait::key_type const key) {
				return !type_trait::key_less(k, key);
			});
	}

	type_trait::buf_size_type
	get_node_id_containing_key_exclusive(type_trait::key_type const k) const
	{
		/*
		 * return the first idx s.t. split[idx] >= k
		 * i.e.: if split[1] == k, then k belongs to tree_nodes[1]
		 */
		return pab_tree::detail::binary_search<k_binary_search_base>(
			this->get_split_slice(), [&](type_trait::key_type const key) {
				return type_trait::key_less(key, k);
			});
	}

	bool validate_key_in_node(type_trait::buf_size_type node_idx,
							  type_trait::key_type const k) const
	{
		if (this->get_split_n() == 0) {
			return true; /* one child: every key belongs to it */
		}
		/* a >= b is !(a < b); a < b is key_less directly. */
		return node_idx == 0 ?
				   type_trait::key_less(k, this->split.front()) :
				   (node_idx == this->get_split_n() ?
						!type_trait::key_less(
							k, this->split[this->get_split_n() - 1]) :
						(!type_trait::key_less(k, this->split[node_idx - 1]) &&
						 type_trait::key_less(k, this->split[node_idx])));
	}

	type_trait::buf_size_type node_n;
	node_arr tree_nodes;
	split_arr split;
	type_trait::interior_aug aug;
};

template <typename InteriorType, typename TT>
static InteriorType *
alloc_interior_node(typename TT::node_seq const &tree_nodes,
					typename TT::split_seq const &split)
{
	InteriorType *o =
		InteriorType::type_trait::node_alloc::template alloc<InteriorType>();
	new (o) InteriorType(tree_nodes, split);
	return o;
}

template <typename InteriorType, typename TT>
static InteriorType *
alloc_interior_node(typename TT::node_slice tree_nodes_slice,
					typename TT::split_slice split_slice)
{
	InteriorType *o =
		InteriorType::type_trait::node_alloc::template alloc<InteriorType>();
	new (o) InteriorType(tree_nodes_slice, split_slice);
	return o;
}

/*
 * TODO: maybe replace the node_arr, ST, and aug_type with template parameters
 */
template <typename InteriorType, typename TT>
static InteriorType *
alloc_interior_node(typename TT::node_slice const &tree_nodes,
					typename TT::split_seq const &split,
					typename TT::interior_aug const &aug)
{
	InteriorType *o =
		InteriorType::type_trait::node_alloc::template alloc<InteriorType>();
	new (o) InteriorType(tree_nodes, split, aug);
	return o;
}

template <typename InteriorType, typename TT>
static InteriorType *alloc_interior_node()
{
	InteriorType *o =
		InteriorType::type_trait::node_alloc::template alloc<InteriorType>();
	new (o) InteriorType();
	return o;
}

template <typename NodeType>
static void free_node(auto node)
{
	NodeType::type_trait::node_alloc::template free<NodeType>(
		static_cast<NodeType *>(node));
}

} /* namespace pab_tree */

#endif /* PABTREE_DEPENDENCE_TREE_NODE_H_ */

#ifndef PABTREE_DEPENDENCE_TYPE_TRAIT_H_
#define PABTREE_DEPENDENCE_TYPE_TRAIT_H_

#include <concepts>
#include <cstdint>
#include <functional>
#include <type_traits>
#include <utility>

#include "pab_tree/dependence/comparator.h"
#include "pab_tree/dependence/concepts.h"
#include "pab_tree/dependence/node_base.h"
#include "parlay/sequence.h"
#include "parlay/slice.h"

namespace pab_tree
{
template <typename EleType, uint BufferSize, typename LeafAugType,
		  typename InteriorAugType, typename NodeType, typename KeyCompare,
		  typename NodeAllocType>
class TypeTrait {
public:
	// element type
	using E = EleType::E;
	using K = typename EleType::K;
	using V = typename EleType::V;
	using Entry = EleType; // abstract type to access E

	// general type
	using Num = Num_Comparator<K>;

	// Key ordering. Every ordering decision in the tree goes through
	// key_less; nothing compares keys with a raw operator.
	using key_compare = KeyCompare;

	static bool key_less(K const &a, K const &b)
	{
		return KeyCompare{}(a, b);
	}

	// a == b under key_compare. Two comparisons in general, but the
	// default and any comparator supplying a static equal() get one.
	static bool key_equal(K const &a, K const &b)
	{
		if constexpr (requires {
						  {
							  KeyCompare::equal(a, b)
						  } -> std::convertible_to<bool>;
					  }) {
			return KeyCompare::equal(a, b);
		} else if constexpr (std::same_as<KeyCompare, std::less<K>>) {
			return a == b;
		} else {
			return !KeyCompare{}(a, b) && !KeyCompare{}(b, a);
		}
	}

	// Order entries by their keys.
	static bool entry_less(E const &a, E const &b)
	{
		return key_less(Entry::get_key(a), Entry::get_key(b));
	}

	using entry_seq = parlay::sequence<E>;
	using entry_slice = parlay::slice<E *, E *>;
	using const_entry_slice = parlay::slice<E const *, E const *>;
	using const_entry_seq = parlay::sequence<E> const;
	using entry_iter = typename parlay::sequence<E>::iterator;

	using key_seq = parlay::sequence<K>;
	using const_key_seq = parlay::sequence<K> const;
	using key_slice = parlay::slice<K *, K *>;
	using const_key_slice = parlay::slice<K const *, K const *>;
	using key_iter = typename parlay::sequence<K>::iterator;

	using Box = std::pair<K, K>;
	using box_seq = parlay::sequence<Box>;

	using leaf_aug = LeafAugType;
	using interior_aug = InteriorAugType;

	using Node = NodeType;
	// Where nodes come from. See parlay_node_alloc in node_base.h.
	using node_alloc = NodeAllocType;
	using node_seq = parlay::sequence<Node *>;
	using node_slice = parlay::slice<Node **, Node **>;
	using const_node_slice = parlay::slice<Node *const *, Node *const *>;

	using pattern_type = std::uint8_t;

	enum leaf_entries_pattern : pattern_type {
		sorted = 0,
		unsorted = 1,
		hybrid = 2,
	};
};

// Defaults for everything btree_type_trait does not require. Inherit and
// override only what you need, then hand it to btree_trait below -- that way
// setting the last parameter does not mean naming the four before it.
struct default_options {
	using leaf_aug = empty_leaf_aug;
	using interior_aug = empty_interior_aug;
	using node = proto_node;
	using node_alloc = parlay_node_alloc;
	// void means std::less<K>. A plain alias, not a member template, so
	// an options struct can be declared inside a function.
	using key_compare = void;
};

// The tree's configuration. Only EleType is required.
//
//   EleType         reads K and V out of your entry; see the entry_type
//                   concept
//   BufferSize      leaf size; a leaf holds BufferSize..4*BufferSize entries
//   LeafAugType     per-leaf augmentation, recomputed on every leaf change
//   InteriorAugType per-interior augmentation, recomputed on every update
//   NodeType        common base of both node types; see the node_base concept
//   KeyCompare      key ordering; everything in the tree goes through it
//   NodeAllocType   where nodes come from; must be thread safe
//
// They are positional, so overriding a later one means naming the earlier
// defaults. The defaults are all spelled `pab_tree::empty_leaf_aug`,
// `empty_interior_aug`, `proto_node`, `std::less<K>`, `parlay_node_alloc`.
template <entry_type EleType, uint BufferSize = 64,
		  typename LeafAugType = empty_leaf_aug,
		  typename InteriorAugType = empty_interior_aug,
		  node_base NodeType = proto_node,
		  typename KeyCompare = std::less<typename EleType::K>,
		  typename NodeAllocType = parlay_node_alloc>
class btree_type_trait
	: public TypeTrait<EleType, BufferSize, LeafAugType, InteriorAugType,
					   NodeType, KeyCompare, NodeAllocType> {
public:
	using BT = TypeTrait<EleType, BufferSize, LeafAugType, InteriorAugType,
						 NodeType, KeyCompare, NodeAllocType>;
	using Split = BT::K;
	using split_seq = parlay::sequence<Split>;
	using split_slice = parlay::slice<Split *, Split *>;
	using split_slice_const = parlay::slice<Split const *, Split const *>;

	static uint const k_buf_md_fac = 2;
	static uint const k_buf_ub_fac = 4;

	using height_type = uint_fast8_t;
	using size_type = uint_fast32_t;
	using buf_size_type = std::conditional_t<
		(BufferSize * k_buf_ub_fac > 65535U), uint_fast32_t,
		std::conditional_t<(BufferSize * k_buf_ub_fac > 255U), uint_fast16_t,
						   uint_fast8_t>>;
	// The augmentation requirements that do not depend on a range type.
	// update_aug/update are called with several different range types, so
	// they are checked where they are called, not here.
	static_assert(std::default_initializable<LeafAugType>,
				  "leaf augmentation must be default constructible");
	static_assert(std::default_initializable<InteriorAugType>,
				  "interior augmentation must be default constructible");
	static_assert(
		requires(size_t n) {
			{
				InteriorAugType::force_parallel(n, n)
			} -> std::convertible_to<bool>;
		},
		"interior augmentation needs static bool force_parallel(size_t, size_t)");

	static constexpr buf_size_type k_buf = BufferSize;
	static constexpr buf_size_type k_buf_lb = BufferSize;
	static constexpr buf_size_type k_buf_md = k_buf_md_fac * BufferSize;
	static constexpr buf_size_type k_buf_ub = k_buf_ub_fac * BufferSize;

	using size_buf_ub_arr = std::array<size_type, k_buf_ub>;
	using buf_size_buf_ub_arr = std::array<buf_size_type, k_buf_ub>;
	using boolean_buf_ub_arr = std::array<bool, k_buf_ub>;

	using entry_arr = std::array<typename BT::E, k_buf_ub>;

	static constexpr uint_fast8_t const k_split_factor = 8;
};

// btree_type_trait by name rather than by position. An alias, so the two
// cannot drift apart:
//
//   struct desc : pab_tree::default_options {
//           using key_compare = std::greater<uint64_t>;
//   };
//   using tt = pab_tree::btree_trait<my_ele, 64, desc>;
template <entry_type EleType, uint BufferSize = 64,
		  typename Options = default_options>
using btree_trait = btree_type_trait<
	EleType, BufferSize, typename Options::leaf_aug,
	typename Options::interior_aug, typename Options::node,
	std::conditional_t<std::is_void_v<typename Options::key_compare>,
					   std::less<typename EleType::K>,
					   typename Options::key_compare>,
	typename Options::node_alloc>;

} // namespace pab_tree

#endif // PABTREE_DEPENDENCE_TYPE_TRAIT_H_

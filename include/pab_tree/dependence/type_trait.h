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
template <class EleType> struct ele_wrapper : EleType {
	using E = typename EleType::E;
	using K = typename EleType::K;
	using V = typename EleType::V;

	static auto get_key(E const e)
	{
		return EleType::get_key(e);
	}
	static auto get_val(E const e)
	{
		return EleType::get_val(e);
	}
	static E to_entry(K const &k, V const &v)
	{
		return EleType::to_entry(k, v);
	};
};

template <typename EleType, uint BufferSize, typename LeafAugType,
		  typename InteriorAugType, typename NodeType, typename KeyCompare>
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

template <entry_type EleType, uint BufferSize,
		  typename LeafAugType = empty_leaf_aug,
		  typename InteriorAugType = empty_interior_aug,
		  typename NodeType = proto_node,
		  typename KeyCompare = std::less<typename EleType::K>>
class btree_type_trait
	: public TypeTrait<ele_wrapper<EleType>, BufferSize, LeafAugType,
					   InteriorAugType, NodeType, KeyCompare> {
public:
	using BT = TypeTrait<ele_wrapper<EleType>, BufferSize, LeafAugType,
						 InteriorAugType, NodeType, KeyCompare>;
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

} // namespace pab_tree

#endif // PABTREE_DEPENDENCE_TYPE_TRAIT_H_

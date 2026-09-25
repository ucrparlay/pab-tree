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
/*
 * Accessors for an entry that already exposes key and val. The tree picks
 * this up on its own, so such an entry needs no adaptor written for it.
 */
template <typename EntryType>
struct plain_entry_adaptor {
	static_assert(keyed_entry<EntryType>,
				  "the entry needs public key and val members");

	using entry_type = EntryType;
	using key_type = decltype(EntryType::key);
	using mapped_type = decltype(EntryType::val);

	static key_type get_key(entry_type const &e)
	{
		return e.key;
	}

	static mapped_type get_val(entry_type const &e)
	{
		return e.val;
	}

	static entry_type to_entry(key_type const &k, mapped_type const &v)
	{
		entry_type e{};
		e.key = k;
		e.val = v;
		return e;
	}
};

/*
 * How the tree reads an entry: the type itself when it is already an
 * adaptor, else the plain accessors above.
 */
template <entry_spec EleType>
using entry_traits_of = std::conditional_t<entry_adaptor<EleType>, EleType,
										   plain_entry_adaptor<EleType>>;

template <typename EleType, uint BufferSize, typename LeafAugType,
		  typename InteriorAugType, typename NodeType, typename KeyCompare,
		  typename NodeAllocType>
class type_trait_base {
public:
	/* element type */
	using entry_traits = entry_traits_of<EleType>; /* reads an entry */
	using entry_type = typename entry_traits::entry_type;
	using key_type = typename entry_traits::key_type;
	using mapped_type = typename entry_traits::mapped_type;

	/* general type */
	using num_comparator = numeric_comparator<key_type>;

	/*
	 * Key ordering. Every ordering decision in the tree goes through
	 * key_less; nothing compares keys with a raw operator.
	 */
	using key_compare = KeyCompare;

	static bool key_less(key_type const &a, key_type const &b)
	{
		return KeyCompare{}(a, b);
	}

	/*
	 * a == b under key_compare. Two comparisons in general, but the
	 * default and any comparator supplying a static equal() get one.
	 */
	static bool key_equal(key_type const &a, key_type const &b)
	{
		if constexpr (requires {
						  {
							  KeyCompare::equal(a, b)
						  } -> std::convertible_to<bool>;
					  }) {
			return KeyCompare::equal(a, b);
		} else if constexpr (std::same_as<KeyCompare, std::less<key_type>>) {
			return a == b;
		} else {
			return !KeyCompare{}(a, b) && !KeyCompare{}(b, a);
		}
	}

	/* Order entries by their keys. */
	static bool entry_less(entry_type const &a, entry_type const &b)
	{
		return key_less(entry_traits::get_key(a), entry_traits::get_key(b));
	}

	using entry_seq = parlay::sequence<entry_type>;
	using entry_slice = parlay::slice<entry_type *, entry_type *>;
	using const_entry_slice =
		parlay::slice<entry_type const *, entry_type const *>;
	using const_entry_seq = parlay::sequence<entry_type> const;
	using entry_iter = typename parlay::sequence<entry_type>::iterator;

	using key_seq = parlay::sequence<key_type>;
	using const_key_seq = parlay::sequence<key_type> const;
	using key_slice = parlay::slice<key_type *, key_type *>;
	using const_key_slice = parlay::slice<key_type const *, key_type const *>;
	using key_iter = typename parlay::sequence<key_type>::iterator;

	using box_type = std::pair<key_type, key_type>;
	using box_seq = parlay::sequence<box_type>;

	using leaf_aug = LeafAugType;
	using interior_aug = InteriorAugType;

	using node_type = NodeType;
	/* Where nodes come from. See parlay_node_alloc in node_base.h. */
	using node_alloc = NodeAllocType;
	using node_seq = parlay::sequence<node_type *>;
	using node_slice = parlay::slice<node_type **, node_type **>;
	using const_node_slice =
		parlay::slice<node_type *const *, node_type *const *>;

	using pattern_type = std::uint8_t;

	enum leaf_entries_pattern : pattern_type {
		sorted = 0,
		unsorted = 1,
		hybrid = 2,
	};
};

/*
 * How batch_delete fixes the levels below a valid root. repair_delete
 * merges thin siblings in place; graft_delete moves the valid subtrees of
 * each invalid one back whole (docs/plan_graft_delete_2026-09.md).
 */
struct repair_delete {};

struct graft_delete {};

/*
 * Defaults for everything btree_type_trait does not require. Inherit and
 * override only what you need, then hand it to btree_trait below -- that way
 * setting the last parameter does not mean naming the four before it.
 */
struct default_options {
	using leaf_aug = empty_leaf_aug;
	using interior_aug = empty_interior_aug;
	using node = proto_node;
	using node_alloc = parlay_node_alloc;
	/*
	 * void means std::less<key_type>. A plain alias, not a member template, so
	 * an options struct can be declared inside a function.
	 */
	using key_compare = void;
	using delete_policy = repair_delete;
};

/*
 * The tree's configuration. Only EleType is required.
 *
 *   EleType         your entry type, if it has public key and val members;
 *                   otherwise an adaptor that reads them, see the
 *                   entry_adaptor concept
 *   BufferSize      leaf size; a leaf holds BufferSize..4*BufferSize entries
 *   LeafAugType     per-leaf augmentation, recomputed on every leaf change
 *   InteriorAugType per-interior augmentation, recomputed on every update
 *   NodeType        common base of both node types; see the node_base concept
 *   KeyCompare      key ordering; everything in the tree goes through it
 *   NodeAllocType   where nodes come from; must be thread safe
 *   DeletePolicy    repair_delete or graft_delete
 *
 * They are positional, so overriding a later one means naming the earlier
 * defaults. The defaults are all spelled `pab_tree::empty_leaf_aug`,
 * `empty_interior_aug`, `proto_node`, `std::less<key_type>`,
 * `parlay_node_alloc` and `repair_delete`.
 */
template <entry_spec EleType, uint BufferSize = 64,
		  typename LeafAugType = empty_leaf_aug,
		  typename InteriorAugType = empty_interior_aug,
		  node_base NodeType = proto_node,
		  typename KeyCompare =
			  std::less<typename entry_traits_of<EleType>::key_type>,
		  typename NodeAllocType = parlay_node_alloc,
		  typename DeletePolicy = repair_delete>
class btree_type_trait
	: public type_trait_base<EleType, BufferSize, LeafAugType, InteriorAugType,
							 NodeType, KeyCompare, NodeAllocType> {
public:
	using base_trait =
		type_trait_base<EleType, BufferSize, LeafAugType, InteriorAugType,
						NodeType, KeyCompare, NodeAllocType>;

	static_assert(std::same_as<DeletePolicy, repair_delete> ||
					  std::same_as<DeletePolicy, graft_delete>,
				  "the delete policy is repair_delete or graft_delete");
	using delete_policy = DeletePolicy;

	using split_type = base_trait::key_type;
	using split_seq = parlay::sequence<split_type>;
	using split_slice = parlay::slice<split_type *, split_type *>;
	using split_slice_const =
		parlay::slice<split_type const *, split_type const *>;

	static uint const k_buf_md_fac = 2;
	static uint const k_buf_ub_fac = 4;

	using height_type = uint_fast8_t;
	using size_type = uint_fast32_t;
	using buf_size_type = std::conditional_t<
		(BufferSize * k_buf_ub_fac > 65535U), uint_fast32_t,
		std::conditional_t<(BufferSize * k_buf_ub_fac > 255U), uint_fast16_t,
						   uint_fast8_t>>;
	/*
	 * The range-independent half of the augmentation contract. The rest is
	 * checked where update_aug/update are called, against the range each
	 * site actually passes.
	 */
	static_assert(leaf_augmentation<LeafAugType>,
				  "leaf augmentation needs a default constructor and reset()");
	static_assert(interior_augmentation<InteriorAugType>,
				  "interior augmentation needs a default constructor, reset() "
				  "and static bool force_parallel(size_t, size_t)");

	static constexpr buf_size_type k_buf = BufferSize;
	static constexpr buf_size_type k_buf_lb = BufferSize;
	static constexpr buf_size_type k_buf_md = k_buf_md_fac * BufferSize;
	static constexpr buf_size_type k_buf_ub = k_buf_ub_fac * BufferSize;

	using size_buf_ub_arr = std::array<size_type, k_buf_ub>;
	using buf_size_buf_ub_arr = std::array<buf_size_type, k_buf_ub>;
	using boolean_buf_ub_arr = std::array<bool, k_buf_ub>;

	using entry_arr = std::array<typename base_trait::entry_type, k_buf_ub>;

	static constexpr uint_fast8_t const k_split_factor = 8;
};

/*
 * btree_type_trait by name rather than by position. An alias, so the two
 * cannot drift apart:
 *
 *   struct desc : pab_tree::default_options {
 *           using key_compare = std::greater<uint64_t>;
 *   };
 *   using tt = pab_tree::btree_trait<my_entry, 64, desc>;
 */
template <entry_spec EleType, uint BufferSize = 64,
		  typename Options = default_options>
using btree_trait = btree_type_trait<
	EleType, BufferSize, typename Options::leaf_aug,
	typename Options::interior_aug, typename Options::node,
	std::conditional_t<std::is_void_v<typename Options::key_compare>,
					   std::less<typename entry_traits_of<EleType>::key_type>,
					   typename Options::key_compare>,
	typename Options::node_alloc, typename Options::delete_policy>;

} /* namespace pab_tree */

#endif /* PABTREE_DEPENDENCE_TYPE_TRAIT_H_ */

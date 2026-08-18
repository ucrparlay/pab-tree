#ifndef PABTREE_DEPENDENCE_CONCEPTS_H_
#define PABTREE_DEPENDENCE_CONCEPTS_H_

#include <concepts>
#include <cstddef>
#include <type_traits>
#include <variant>

namespace pab_tree
{

// True when the augmentation carries state, so the tree can skip maintaining
// it entirely when it does not.
template <typename T>
concept node_has_non_trivial_aug =
	(!std::is_empty_v<T>) && (!std::same_as<T, std::monostate>);

// What the node base must provide. Both node types derive from it: the tree
// reads is_leaf to pick the downcast and size to answer range queries without
// descending, and rebalancing writes size.
template <typename T>
concept node_base =
	std::constructible_from<T, bool, size_t> && requires(T t, size_t n) {
		{ t.is_leaf } -> std::convertible_to<bool>;
		{ t.size } -> std::convertible_to<size_t>;
		t.size = n;
	};

// Tags a pab_tree so generic test code can detect one.
template <typename T>
concept is_pab_tree = requires(T t) {
	{ t.pab_tree_tag() } -> std::same_as<void>;
};

// What a user's element adaptor must provide. entry_type is the stored
// entry, key_type the key it is ordered by, mapped_type the payload.
template <typename T>
concept entry_adaptor =
	requires(typename T::entry_type e, typename T::key_type k,
			 typename T::mapped_type v) {
		typename T::entry_type;
		typename T::key_type;
		typename T::mapped_type;
		{ T::get_key(e) } -> std::convertible_to<typename T::key_type>;
		{ T::get_val(e) } -> std::convertible_to<typename T::mapped_type>;
		{ T::to_entry(k, v) } -> std::convertible_to<typename T::entry_type>;
		{ k < k } -> std::convertible_to<bool>;
	};

// What an augmentation must provide regardless of the range it is handed.
// The range-dependent halves are separate below, because the constructor and
// update_aug are called with different range types.
template <typename A>
concept leaf_augmentation =
	std::default_initializable<A> && requires(A a) { a.reset(); };

template <typename A>
concept interior_augmentation =
	std::default_initializable<A> && requires(A a, size_t sz) {
		a.reset();
		{ A::force_parallel(sz, sz) } -> std::convertible_to<bool>;
	};

// Checked where the call happens, against the range actually passed.
template <typename A, typename Range>
concept leaf_aug_constructible_from = std::constructible_from<A, Range>;

template <typename A, typename Range>
concept leaf_aug_updatable_with = requires(A a, Range r) { a.update_aug(r); };

template <typename A, typename Nodes, typename Splits>
concept interior_aug_updatable_with =
	requires(A a, Nodes const &n, Splits const &s) { a.update(n, s); };

} // namespace pab_tree

#endif // PABTREE_DEPENDENCE_CONCEPTS_H_

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

// What a user's element adaptor must provide. E is the stored entry, K the key
// it is ordered by, V the payload.
template <typename T>
concept entry_type =
	requires(typename T::E e, typename T::K k, typename T::V v) {
		typename T::E;
		typename T::K;
		typename T::V;
		{ T::get_key(e) } -> std::convertible_to<typename T::K>;
		{ T::get_val(e) } -> std::convertible_to<typename T::V>;
		{ T::to_entry(k, v) } -> std::convertible_to<typename T::E>;
		{ k < k } -> std::convertible_to<bool>;
	};

// What a leaf augmentation must provide. Called with the leaf's entries.
template <typename A, typename Slice>
concept leaf_augmentation =
	std::default_initializable<A> && std::constructible_from<A, Slice> &&
	requires(A a, Slice s) { a.update_aug(s); };

// What an interior augmentation must provide. update() is called with the
// node's children and split keys.
template <typename A, typename Nodes, typename Splits>
concept interior_augmentation =
	std::default_initializable<A> &&
	requires(A a, Nodes const &n, Splits const &s, size_t sz) {
		a.update(n, s);
		{ A::force_parallel(sz, sz) } -> std::convertible_to<bool>;
	};

} // namespace pab_tree

#endif // PABTREE_DEPENDENCE_CONCEPTS_H_

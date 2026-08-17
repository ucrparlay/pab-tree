#ifndef PABTREE_DEPENDENCE_CONCEPTS_H_
#define PABTREE_DEPENDENCE_CONCEPTS_H_

#include <concepts>
#include <type_traits>
#include <variant>

namespace pab_tree {

// True when the augmentation carries state, so the tree can skip maintaining
// it entirely when it does not.
template<typename T>
concept node_has_non_trivial_aug =
  (!std::is_empty_v<T>) && (!std::same_as<T, std::monostate>);

// Tags a pab_tree so generic test code can detect one.
template<typename T>
concept is_pab_tree = requires(T t) {
  { t.pab_tree_tag() } -> std::same_as<void>;
};

} // namespace pab_tree

#endif // PABTREE_DEPENDENCE_CONCEPTS_H_

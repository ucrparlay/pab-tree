#ifndef PABTREE_DEPENDENCE_NODE_BASE_H_
#define PABTREE_DEPENDENCE_NODE_BASE_H_

#include <cstddef>

namespace pab_tree
{

// Base of every node. is_leaf selects the downcast; size is the subtree entry
// count, which the range queries read without descending.
struct proto_node {
	proto_node()
		: is_leaf{ false }
		, size{ 0 } {};
	proto_node(bool _is_leaf, size_t _size)
		: is_leaf{ _is_leaf }
		, size{ _size } {};

	bool is_leaf;
	size_t size;
};

// Defaults for a tree that carries no augmentation. Both are empty, so
// node_has_non_trivial_aug is false and every update_aug call compiles away.
struct empty_leaf_aug {
	empty_leaf_aug() = default;
	template <typename Slice> explicit empty_leaf_aug(Slice)
	{
	}
	template <typename Slice> void update_aug(Slice)
	{
	}
	void reset()
	{
	}
};

struct empty_interior_aug {
	empty_interior_aug() = default;
	template <typename Nodes, typename Splits>
	void update(Nodes const &, Splits const &)
	{
	}
	[[nodiscard]] static bool force_parallel(size_t, size_t)
	{
		return true;
	}
	void reset()
	{
	}
};

} // namespace pab_tree

#endif // PABTREE_DEPENDENCE_NODE_BASE_H_

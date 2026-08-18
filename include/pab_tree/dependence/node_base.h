#ifndef PABTREE_DEPENDENCE_NODE_BASE_H_
#define PABTREE_DEPENDENCE_NODE_BASE_H_

#include <cstddef>

#include <parlay/alloc.h>

namespace pab_tree
{

// How nodes are obtained and released. alloc returns raw storage; the
// caller placement-news into it, and free is called after the destructor.
// A pool is what makes this fast -- the tree allocates one node per leaf
// and per interior, so a general-purpose allocator shows up immediately.
struct parlay_node_alloc {
	template <typename T>
	static T *alloc()
	{
		return parlay::type_allocator<T>::alloc();
	}

	template <typename T>
	static void free(T *p)
	{
		parlay::type_allocator<T>::retire(p);
	}
};

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
	template <typename Slice>
	explicit empty_leaf_aug(Slice)
	{
	}
	template <typename Slice>
	void update_aug(Slice)
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

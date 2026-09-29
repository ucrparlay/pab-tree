#ifndef PABTREE_B_TREE_IMPL_VALIDATION_HPP_
#define PABTREE_B_TREE_IMPL_VALIDATION_HPP_

#include <cassert>

#include <parlay/parallel.h>

#include <concepts>
#include <cwchar>

#include "pab_tree/b_tree.h"
#include "parlay/primitives.h"

namespace pab_tree
{
/* Returns the subtree box and whether every check passed. */
template <typename TT>
template <bool CheckSize>
std::pair<typename TT::box_type, bool>
btree<TT>::verify_tree_property(node_type const *node) const
{
	if (node == nullptr) {
		return { get_empty_box<TT>(), false };
	}

	if (node->is_leaf) {
		leaf_type const *leaf = static_cast<leaf_type const *>(node);
		bool ok = true;
		if constexpr (CheckSize) {
			assert(node == this->root_ ||
				   (leaf->get_ele().size() >= TT::k_buf_lb &&
					leaf->get_ele().size() <= TT::k_buf_ub));
			ok = ok && (node == this->root_ || (leaf->size >= TT::k_buf_lb &&
												leaf->size <= TT::k_buf_ub));
		}
		assert(!leaf->is_sorted ||
			   parlay::is_sorted(leaf->get_ele(),
								 [&](auto const &e1, auto const &e2) {
									 return TT::entry_less(e1, e2);
								 }));
		ok = ok && (!leaf->is_sorted ||
					parlay::is_sorted(leaf->get_ele_slice(),
									  [&](auto const &e1, auto const &e2) {
										  return TT::entry_less(e1, e2);
									  }));
		return { pab_tree::get_box<TT>(leaf->get_ele_slice()), ok };
	}

	auto const *interior = static_cast<interior_type const *>(node);

	/* ensure the split number is 1 less than the child number */
	assert(node == this->root_ || (interior->get_node_n() >= TT::k_buf_lb &&
								   interior->get_node_n() <= TT::k_buf_ub));

	/* ensure the split is increasing */
	assert(parlay::is_sorted(
		interior->get_split_slice(),
		[&](TT::key_type a, TT::key_type b) { return TT::key_less(a, b); }));

	bool node_ok =
		(node == this->root_ || (interior->get_node_n() >= TT::k_buf_lb &&
								 interior->get_node_n() <= TT::k_buf_ub)) &&
		interior->get_node_n() > 0 &&
		parlay::is_sorted(
			interior->get_split_slice(),
			[&](TT::key_type a, TT::key_type b) { return TT::key_less(a, b); });

	/* check the box of each child node falls within the split between */
	parlay::sequence<typename TT::box_type> box_seq(interior->get_node_n());
	parlay::sequence<bool> ok_seq(interior->get_node_n(), true);
	parlay::sequence<size_t> size_seq(interior->get_node_n(), 0);
	parlay::parallel_for(0, interior->get_node_n(), [&](size_t i) {
		auto [child_box, child_ok] =
			verify_tree_property<CheckSize>(interior->tree_nodes[i]);
		box_seq[i] = child_box;
		ok_seq[i] = child_ok && interior->tree_nodes[i] != nullptr;
		size_seq[i] = interior->tree_nodes[i] == nullptr ?
						  0 :
						  interior->tree_nodes[i]->size;
		[[maybe_unused]] auto child = interior->tree_nodes[i];
		auto box = box_seq[i];

		/*
		 * A single child means no split keys at all, so there is
		 * nothing to compare against; split[0] would be uninitialised.
		 * node_op.hpp:143 and :612 skip the same case.
		 */
		if (interior->get_node_n() == 1) {
			return;
		}

		if (i == 0) {
			assert(TT::key_less(box.second, interior->split[0]));
			ok_seq[i] =
				ok_seq[i] && TT::key_less(box.second, interior->split[0]);
		} else if (i == static_cast<size_t>(interior->get_node_n()) - 1) {
			assert(!TT::key_less(box.first, interior->split[i - 1]));
			ok_seq[i] =
				ok_seq[i] && !TT::key_less(box.first, interior->split[i - 1]);
		} else {
			if (TT::key_less(box.first, interior->split[i - 1]) ||
				!TT::key_less(box.second, interior->split[i])) {
				std::cout << "validate: box outside split" << std::endl;
				std::cout << "box_type: [" << box.first << ", " << box.second
						  << "], Splits: [" << interior->split[i - 1] << ", "
						  << interior->split[i] << "]" << std::endl;
				assert(false);
			}
			assert(!TT::key_less(box.first, interior->split[i - 1]) &&
				   TT::key_less(box.second, interior->split[i]));
			ok_seq[i] = ok_seq[i] &&
						!TT::key_less(box.first, interior->split[i - 1]) &&
						TT::key_less(box.second, interior->split[i]);
		}
	});

	/*
	 * interior->size must equal the sum of the child sizes. range_count and
	 * range_report derives disjoint parallel output slices from it.
	 */
	if constexpr (CheckSize) {
		size_t const child_sum = parlay::reduce(size_seq);
		if (child_sum != interior->size) {
			std::cout << "Validation failed: interior size " << interior->size
					  << " != sum of children " << child_sum << std::endl;
			node_ok = false;
		}
	}

	bool const children_ok = parlay::reduce(
		ok_seq, parlay::binary_op(std::logical_and<bool>(), true));

	return { pab_tree::get_box<TT>(box_seq), node_ok && children_ok };
}

template <typename TT>
bool btree<TT>::verify_unoverlapped_tree(node_type const *node,
										 TT::node_slice node_slice) const
{
	int cnt = 0;
	for (int i = 0; i < node_slice.size(); i++) {
		auto child = node_slice[i];
		cnt += check_contain_node(node, child) ? 1 : 0;
	}
	assert(cnt == 1);
	return cnt == 1;
}

/* check whether target is in the subtree rooted at node */
template <typename TT>
bool btree<TT>::check_contain_node(node_type const *node,
								   node_type const *target) const
{
	if (node == target) {
		return true;
	}

	if (node->is_leaf) {
		return false;
	}

	auto interior = static_cast<interior_type const *>(node);
	boolean_buf_ub_arr contain_arr;

	parlay::parallel_for(0, interior->get_node_n(), [&](buf_size_type i) {
		contain_arr[i] = check_contain_node(interior->tree_nodes[i], target);
	});

	return parlay::reduce(
		parlay::make_slice(contain_arr.begin(),
						   contain_arr.begin() + interior->get_node_n()),
		parlay::binary_op([&](bool a, bool b) { return a | b; }, bool()));
}

template <typename TT>
TT::height_type btree<TT>::check_height(node_type const *node) const
{
	if (node->is_leaf) {
		return 1;
	}

	auto interior = static_cast<interior_type const *>(node);
	parlay::sequence<typename TT::height_type> heights(interior->get_node_n());

	parlay::parallel_for(0, interior->get_node_n(), [&](size_t i) {
		heights[i] = check_height(interior->tree_nodes[i]);
	});

	assert(parlay::all_of(heights, [&](auto h) { return h == heights[0]; }));

	return heights[0] + 1;
}

template <typename TT>
template <typename LeafType, typename InteriorType>
int btree<TT>::get_height(node_type const *node)
{
	if (node->is_leaf) {
		return 0;
	}

	InteriorType const *interior = static_cast<InteriorType const *>(node);
	parlay::sequence<int> heights(interior->get_node_n());

	for (int i = 0; i < interior->get_node_n(); i++) {
		heights[i] =
			get_height<LeafType, InteriorType>(interior->tree_nodes[i]);
	}

	return *parlay::max_element(heights) + 1;
}

template <typename TT>
template <typename LeafType, typename InteriorType>
int btree<TT>::get_tree_nodes_num(node_type const *node)
{
	if (node->is_leaf) {
		return 1;
	}

	auto const *interior = static_cast<InteriorType const *>(node);

	int sum = 0;
	for (int i = 0; i < interior->get_node_n(); i++) {
		sum +=
			get_tree_nodes_num<LeafType, InteriorType>(interior->tree_nodes[i]);
	}

	return sum + 1;
}

template <typename TT>
template <bool CheckSize, bool Verbose>
bool btree<TT>::validate(node_type const *node) const
{
	if (node == nullptr) {
		node = this->root_;
	}

	/* An empty tree is valid. */
	if (node == nullptr) {
		return true;
	}

	if constexpr (Verbose) {
		std::cerr << std::endl;
		std::cerr << ">>> begin validate tree\n" << std::flush;
	}

	auto const [box, ok] = verify_tree_property<CheckSize>(node);
	(void)box;

	/* typename TT::height_type height = */
	check_height(node);

	if constexpr (Verbose) {
		std::cerr << "<<< end validate tree\n" << std::flush;
	}
	return ok;
}

template <typename TT>
std::pair<typename TT::size_type, typename TT::size_type>
btree<TT>::count_sorted_leaves(node_type const *node)
{
	if (node->is_leaf) {
		leaf_type const *leaf = static_cast<leaf_type const *>(node);
		return { leaf->is_sorted ? size_type(1) : size_type(0), size_type(1) };
	}

	auto const *interior = static_cast<interior_type const *>(node);
	auto const node_n = interior->get_node_n();
	parlay::sequence<std::pair<size_type, size_type>> counts(node_n);
	parlay::parallel_for(0, node_n, [&](size_t i) {
		counts[i] = count_sorted_leaves(interior->tree_nodes[i]);
	});

	return parlay::reduce(counts,
						  parlay::binary_op(
							  [](std::pair<size_type, size_type> a,
								 std::pair<size_type, size_type> b) {
								  return std::pair<size_type, size_type>{
									  a.first + b.first, a.second + b.second
								  };
							  },
							  std::pair<size_type, size_type>{ 0, 0 }));
}

template <typename TT>
double btree<TT>::sorted_leaf_ratio(node_type const *node) const
{
	if (node == nullptr) {
		node = this->root_;
	}
	if (node == nullptr) {
		return 0.0;
	}

	auto const [sorted_n, total_n] = count_sorted_leaves(node);
	return total_n == 0 ?
			   0.0 :
			   static_cast<double>(sorted_n) / static_cast<double>(total_n);
}

} /* namespace pab_tree */

#endif /* PABTREE_B_TREE_IMPL_VALIDATION_HPP_ */

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
// Returns the subtree box and whether every check passed.
template <typename type_trait>
template <bool CheckSize>
std::pair<typename type_trait::box_type, bool>
btree<type_trait>::verify_tree_property(node_type const *T) const
{
	if (T == nullptr) {
		return { get_empty_box<type_trait>(), false };
	}

	if (T->is_leaf) {
		leaf_type const *TL = static_cast<leaf_type const *>(T);
		bool ok = true;
		if constexpr (CheckSize) {
			assert(T == this->root_ ||
				   (TL->get_ele().size() >= type_trait::k_buf_lb &&
					TL->get_ele().size() <= type_trait::k_buf_ub));
			ok = ok && (T == this->root_ || (TL->size >= type_trait::k_buf_lb &&
											 TL->size <= type_trait::k_buf_ub));
		}
		assert(!TL->is_sorted ||
			   parlay::is_sorted(TL->get_ele(),
								 [&](auto const &e1, auto const &e2) {
									 return type_trait::entry_less(e1, e2);
								 }));
		ok = ok && (!TL->is_sorted ||
					parlay::is_sorted(TL->get_ele_slice(),
									  [&](auto const &e1, auto const &e2) {
										  return type_trait::entry_less(e1, e2);
									  }));
		return { pab_tree::get_box<type_trait>(TL->get_ele_slice()), ok };
	}

	auto const *TI = static_cast<interior_type const *>(T);

	// ensure the split number is 1 less than the child number
	assert(T == this->root_ || (TI->get_node_n() >= type_trait::k_buf_lb &&
								TI->get_node_n() <= type_trait::k_buf_ub));

	// ensure the split is increasing
	assert(
		parlay::is_sorted(TI->get_split_slice(),
						  [&](type_trait::key_type a, type_trait::key_type b) {
							  return type_trait::key_less(a, b);
						  }));

	bool node_ok =
		(T == this->root_ || (TI->get_node_n() >= type_trait::k_buf_lb &&
							  TI->get_node_n() <= type_trait::k_buf_ub)) &&
		TI->get_node_n() > 0 &&
		parlay::is_sorted(TI->get_split_slice(),
						  [&](type_trait::key_type a, type_trait::key_type b) {
							  return type_trait::key_less(a, b);
						  });

	// check the box of each child node falls within the split between
	parlay::sequence<typename type_trait::box_type> box_seq(TI->get_node_n());
	parlay::sequence<bool> ok_seq(TI->get_node_n(), true);
	parlay::sequence<size_t> size_seq(TI->get_node_n(), 0);
	parlay::parallel_for(0, TI->get_node_n(), [&](size_t i) {
		auto [child_box, child_ok] =
			verify_tree_property<CheckSize>(TI->tree_nodes[i]);
		box_seq[i] = child_box;
		ok_seq[i] = child_ok && TI->tree_nodes[i] != nullptr;
		size_seq[i] =
			TI->tree_nodes[i] == nullptr ? 0 : TI->tree_nodes[i]->size;
		[[maybe_unused]] auto node = TI->tree_nodes[i];
		auto box = box_seq[i];

		// A single child means no split keys at all, so there is
		// nothing to compare against; split[0] would be uninitialised.
		// node_op.hpp:143 and :612 skip the same case.
		if (TI->get_node_n() == 1) {
			return;
		}

		if (i == 0) {
			assert(type_trait::key_less(box.second, TI->split[0]));
			ok_seq[i] =
				ok_seq[i] && type_trait::key_less(box.second, TI->split[0]);
		} else if (i == static_cast<size_t>(TI->get_node_n()) - 1) {
			assert(!type_trait::key_less(box.first, TI->split[i - 1]));
			ok_seq[i] =
				ok_seq[i] && !type_trait::key_less(box.first, TI->split[i - 1]);
		} else {
			if (type_trait::key_less(box.first, TI->split[i - 1]) ||
				!type_trait::key_less(box.second, TI->split[i])) {
				std::cout << "validate: box outside split" << std::endl;
				std::cout << "box_type: [" << box.first << ", " << box.second
						  << "], Splits: [" << TI->split[i - 1] << ", "
						  << TI->split[i] << "]" << std::endl;
				assert(false);
			}
			assert(!type_trait::key_less(box.first, TI->split[i - 1]) &&
				   type_trait::key_less(box.second, TI->split[i]));
			ok_seq[i] = ok_seq[i] &&
						!type_trait::key_less(box.first, TI->split[i - 1]) &&
						type_trait::key_less(box.second, TI->split[i]);
		}
	});

	// TI->size must equal the sum of the child sizes. range_count and
	// range_report derives disjoint parallel output slices from it.
	if constexpr (CheckSize) {
		size_t const child_sum = parlay::reduce(size_seq);
		if (child_sum != TI->size) {
			std::cout << "Validation failed: interior size " << TI->size
					  << " != sum of children " << child_sum << std::endl;
			node_ok = false;
		}
	}

	bool const children_ok = parlay::reduce(
		ok_seq, parlay::binary_op(std::logical_and<bool>(), true));

	return { pab_tree::get_box<type_trait>(box_seq), node_ok && children_ok };
}

template <typename type_trait>
bool btree<type_trait>::verify_unoverlapped_tree(
	node_type const *T, type_trait::node_slice node_slice) const
{
	int cnt = 0;
	for (int i = 0; i < node_slice.size(); i++) {
		auto node = node_slice[i];
		cnt += check_contain_node(T, node) ? 1 : 0;
	}
	assert(cnt == 1);
	return cnt == 1;
}

// check whether node P is in the tree T
template <typename type_trait>
bool btree<type_trait>::check_contain_node(node_type const *T,
										   node_type const *P) const
{
	if (T == P) {
		return true;
	}

	if (T->is_leaf) {
		return false;
	}

	auto TI = static_cast<interior_type const *>(T);
	boolean_buf_ub_arr contain_arr;

	parlay::parallel_for(0, TI->get_node_n(), [&](buf_size_type i) {
		contain_arr[i] = check_contain_node(TI->tree_nodes[i], P);
	});

	return parlay::reduce(
		parlay::make_slice(contain_arr.begin(),
						   contain_arr.begin() + TI->get_node_n()),
		parlay::binary_op([&](bool a, bool b) { return a | b; }, bool()));
}

template <typename type_trait>
type_trait::height_type
btree<type_trait>::check_height(node_type const *T) const
{
	if (T->is_leaf) {
		return 1;
	}

	auto TI = static_cast<interior_type const *>(T);
	parlay::sequence<typename type_trait::height_type> heights(
		TI->get_node_n());

	parlay::parallel_for(0, TI->get_node_n(), [&](size_t i) {
		heights[i] = check_height(TI->tree_nodes[i]);
	});

	assert(parlay::all_of(heights, [&](auto h) { return h == heights[0]; }));

	return heights[0] + 1;
}

template <typename type_trait>
template <typename leaf_type, typename interior_type>
int btree<type_trait>::get_height(node_type const *T)
{
	if (T->is_leaf) {
		return 0;
	}

	interior_type const *TI = static_cast<interior_type const *>(T);
	parlay::sequence<int> heights(TI->get_node_n());

	for (int i = 0; i < TI->get_node_n(); i++) {
		heights[i] = get_height<leaf_type, interior_type>(TI->tree_nodes[i]);
	}

	return *parlay::max_element(heights) + 1;
}

template <typename type_trait>
template <typename leaf_type, typename interior_type>
int btree<type_trait>::get_tree_nodes_num(node_type const *T)
{
	if (T->is_leaf) {
		return 1;
	}

	auto const *TI = static_cast<interior_type const *>(T);

	int sum = 0;
	for (int i = 0; i < TI->get_node_n(); i++) {
		sum += get_tree_nodes_num<leaf_type, interior_type>(TI->tree_nodes[i]);
	}

	return sum + 1;
}

template <typename type_trait>
template <bool CheckSize, bool Verbose>
bool btree<type_trait>::validate(node_type const *T) const
{
	if (T == nullptr) {
		T = this->root_;
	}

	// An empty tree is valid.
	if (T == nullptr) {
		return true;
	}

	if constexpr (Verbose) {
		std::cerr << std::endl;
		std::cerr << ">>> begin validate tree\n" << std::flush;
	}

	auto const [box, ok] = verify_tree_property<CheckSize>(T);
	(void)box;

	// typename type_trait::height_type height =
	check_height(T);

	if constexpr (Verbose) {
		std::cerr << "<<< end validate tree\n" << std::flush;
	}
	return ok;
}

template <typename type_trait>
std::pair<typename type_trait::size_type, typename type_trait::size_type>
btree<type_trait>::count_sorted_leaves(node_type const *T)
{
	if (T->is_leaf) {
		leaf_type const *TL = static_cast<leaf_type const *>(T);
		return { TL->is_sorted ? size_type(1) : size_type(0), size_type(1) };
	}

	auto const *TI = static_cast<interior_type const *>(T);
	auto const node_n = TI->get_node_n();
	parlay::sequence<std::pair<size_type, size_type>> counts(node_n);
	parlay::parallel_for(0, node_n, [&](size_t i) {
		counts[i] = count_sorted_leaves(TI->tree_nodes[i]);
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

template <typename type_trait>
double btree<type_trait>::sorted_leaf_ratio(node_type const *T) const
{
	if (T == nullptr) {
		T = this->root_;
	}
	if (T == nullptr) {
		return 0.0;
	}

	auto const [sorted_n, total_n] = count_sorted_leaves(T);
	return total_n == 0 ?
			   0.0 :
			   static_cast<double>(sorted_n) / static_cast<double>(total_n);
}

template <typename type_trait>
std::tuple<bool, bool, typename type_trait::size_type>
btree<type_trait>::check_tree_aug_after_delete(node_type *T)
{
	if (T->is_leaf) {
		return { get_node_invalid_status(T), node_contain_invalid_subtree(T),
				 get_node_reinsert_size(T) };
	}

	auto TI = static_cast<interior_type *>(T);
	auto node_n = TI->get_node_n();
	parlay::sequence<int> invalid_seq(node_n);
	parlay::sequence<typename type_trait::size_type> invalid_subtree_num_seq(
		node_n);
	parlay::sequence<typename type_trait::size_type> reinsert_size_seq(node_n);
	parlay::parallel_for(0, node_n, [&](size_t i) {
		auto [invalid, invalid_subtree_num, reinsert_size] =
			check_tree_aug_after_delete(TI->tree_nodes[i]);
		invalid_seq[i] = invalid ? 1 : 0;
		invalid_subtree_num_seq[i] = invalid_subtree_num;
		reinsert_size_seq[i] = reinsert_size;
	});

	auto sum_invalid = parlay::reduce(invalid_seq);
	auto sum_invalid_subtree_num = parlay::reduce(invalid_subtree_num_seq);
	auto sum_reinsert_size = parlay::reduce(reinsert_size_seq);

	if (T == this->root_) {
		assert(TI->valid == ((TI->get_node_n() - sum_invalid) > 0));
	} else {
		assert(TI->valid ==
			   ((TI->get_node_n() - sum_invalid) >= type_trait::k_buf_lb));
	}

	if (node_is_valid(T)) {
		assert(sum_invalid_subtree_num == node_contain_invalid_subtree(T));
		assert(sum_reinsert_size == get_node_reinsert_size(T));
	} else {
		assert(node_contain_invalid_subtree(T) == 1);
		assert(get_node_reinsert_size(T) == TI->size);
	}

	return { get_node_invalid_status(T),
			 node_is_valid(T) ? sum_invalid_subtree_num : 1,
			 node_is_valid(T) ? sum_reinsert_size : TI->size };
}

} // namespace pab_tree

#endif // PABTREE_B_TREE_IMPL_VALIDATION_HPP_

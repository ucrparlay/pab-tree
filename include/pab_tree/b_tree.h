#ifndef PABTREE_B_TREE_H_
#define PABTREE_B_TREE_H_

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <ostream>
#include <tuple>
#include <utility>

#include <parlay/primitives.h>
#include <parlay/sequence.h>
#include <parlay/slice.h>
#include <parlay/utilities.h>

#include "pab_tree/dependence/box_op.h"
#include "pab_tree/dependence/comparator.h"
#include "pab_tree/dependence/tree_node.h"
#include "pab_tree/dependence/type_trait.h"

namespace pab_tree
{
// Basetree
template <class TypeTrait> class btree {
public:
	using TT = TypeTrait;
	// TODO: Do we really need to define these types again?

	using E = typename TT::E;
	using K = typename TT::K;
	using V = typename TT::V;
	using Entry = TT::Entry;

	using Node = TT::Node;
	using Num = typename TT::Num;
	using entry_slice = typename TT::entry_slice;
	using Split = typename TT::Split;
	using node_split_seq_pair =
		std::pair<typename TT::node_seq, typename TT::split_seq>;

	using node_split_pair = std::pair<Node *, Split>;
	using node_split_pair_seq = parlay::sequence<node_split_pair>;
	using node_split_pair_slice =
		parlay::slice<node_split_pair *, node_split_pair *>;
	using node_seq_list = parlay::sequence<typename TT::node_seq>;
	using split_seq_list = parlay::sequence<typename TT::split_seq>;

	using leaf_aug = typename TT::leaf_aug;
	using interior_aug = typename TT::interior_aug;

	using input_range_type = TT::entry_seq;
	using input_delete_range_type = TT::key_seq;

	using size_buf_ub_arr = typename TT::size_buf_ub_arr;
	using buf_size_buf_ub_arr = typename TT::buf_size_buf_ub_arr;
	using boolean_buf_ub_arr = typename TT::boolean_buf_ub_arr;
	using buf_size_type = TT::buf_size_type;
	using size_type = TT::size_type;

	// tree structure
	static constexpr uint_fast16_t const k_serial_build_cutoff = 512;

	// constructor and destructor
	btree() = default;

	// Constrained so it cannot outrank the copy constructor.
	template <typename Range>
		requires(!std::same_as<std::remove_cvref_t<Range>, btree>)
	explicit btree(Range &&in)
	{
		this->build(std::forward<Range>(in));
	}

	// copy constructor
	// copy @other
	btree(const btree &other)
		: root_(other.root_ ? this->copy_tree_recursive(other.root_) : nullptr)
	{
	}

	// copy assignment operator
	// copy @other, will launch a deep copy
	btree &operator=(const btree &other)
	{
		if (this == &other) {
			return *this;
		}

		this->delete_tree(); // delete current tree
		this->root_ =
			other.root_ ? this->copy_tree_recursive(other.root_) : nullptr;
		return *this;
	}

	// move constructor
	btree(btree &&other) noexcept
		: root_(other.root_)
	{
		other.root_ = nullptr;
	}

	// move assignment operator
	btree &operator=(btree &&other) noexcept
	{
		if (this == &other) {
			return *this;
		}

		this->delete_tree(); // delete current tree
		this->root_ = other.root_; // steal
		other.root_ = nullptr;

		return *this;
	}

	// destructor
	~btree()
	{
		this->delete_tree();
	}

	// PARA: interior node type
	struct btree_interior_node : multi_way_node<TT> {
		using BaseNode = multi_way_node<TT>;

		btree_interior_node(TT::node_seq const &_tree_nodes,
							TT::split_seq const &_split)
			: BaseNode(_tree_nodes, _split, typename TT::interior_aug())
			, valid(true)
			, contain_invalid_subtree(0)
			, reinsert_size(0)
		{
		} // TODO:finish AT

		btree_interior_node(TT::node_slice _tree_nodes_slice,
							TT::split_slice _split_slice)
			: BaseNode(_tree_nodes_slice, _split_slice,
					   typename TT::interior_aug())
			, valid(true)
			, contain_invalid_subtree(0)
			, reinsert_size(0)
		{
		} // TODO:finish AT

		btree_interior_node(TT::node_seq const &_tree_nodes,
							TT::split_seq const &_split,
							TT::interior_aug const &_aug)
			: BaseNode(_tree_nodes, _split, _aug)
			, valid(true)
			, contain_invalid_subtree(0)
			, reinsert_size(0)
		{
		}

		// Gated so an empty augmentation compiles to nothing.
		void update_aug()
		{
			if constexpr (node_has_non_trivial_aug<typename TT::interior_aug>) {
				this->aug.update(this->get_node_slice(),
								 this->get_split_slice());
			}
		}

		void set_parallel_flag(bool const flag)
		{
			this->aug.set_parallel_flag(flag);
		}

		void reset_parallel_flag()
		{
			this->aug.reset_parallel_flag();
		}

		auto get_parallel_flag_ini_status() -> bool
		{
			return this->aug.get_parallel_flag_ini_status();
		}

		[[nodiscard]] auto force_parallel() const -> bool
		{
			return this->aug.force_parallel(this->size, k_serial_build_cutoff);
		}

		void reset_deletion_flag()
		{
			this->valid = true;
			this->contain_invalid_subtree = 0;
			this->reinsert_size = 0;
		}

		bool valid;
		bool contain_invalid_subtree;
		size_type reinsert_size;
	};

	using Leaf = leaf_node<TT, parlay::move_assign_tag>;
	using Interior = btree_interior_node;

	// ---- public API ----
	struct default_hybrid_pred {
		template <typename InputNode, typename Range>
		bool operator()(InputNode const *T, Range In) const noexcept
		{
			// Knuth multiplicative hash over (node_size +
			// batch_size); XOR in low address bits from begin/end
			// to break correlation across call sites.
			auto const v =
				(static_cast<size_t>(T->size) + In.size()) * 2654435761ULL ^
				(reinterpret_cast<uintptr_t>(In.begin()) >> 3) ^
				(reinterpret_cast<uintptr_t>(In.end()) >> 3);
			return v & 1;
		}
	};

	// build tree
	// Keys must be distinct. Duplicates put elements on the wrong side of a
	// split key, where no descent reaches them; debug builds assert.
	template <typename Range> void build(Range &&In);
	template <typename Range> void batch_insert(Range &&In);
	template <typename Range, typename LeafSortPred = default_hybrid_pred>
	void
	batch_insert(Range &&In, const TT::pattern_type leaf_entries_pattern,
				 const bool sorted_input,
				 LeafSortPred const &leaf_sort_pred = default_hybrid_pred{});
	template <typename Range> void batch_delete(Range &&In);
	template <typename Range, typename LeafSortPred = default_hybrid_pred>
	void
	batch_delete(Range &&In, const TT::pattern_type leaf_entries_pattern,
				 const bool sorted_input,
				 LeafSortPred const &leaf_sort_pred = default_hybrid_pred{});
	void delete_tree();
	template <typename Range> void get_entries(Range &Out) const;
	template <bool CheckSize = true, bool Verbose = true>
	[[nodiscard]] bool validate(Node const *T = nullptr) const;
	// Ratio of sorted leaves to total leaves in the subtree rooted at T
	// (defaults to the whole tree). Returns 0.0 for an empty tree.
	double sorted_leaf_ratio(Node const *T = nullptr) const;
	auto contain(K const &k) const -> bool;
	auto range_count(TT::Box query_box) const -> size_type;
	// Writes the entries in [first, second) into out and returns how many.
	// PRECONDITION: out holds at least range_count(query_box) entries. A
	// smaller out overruns; debug builds assert.
	template <typename Range>
	auto range_report(TT::Box query_box, Range &&out) const -> size_type;

	// ---- implementation ----
private:
	// Order by key. Element types are not required to define operator<.
	static constexpr auto key_less = [](E const &a, E const &b) {
		return Entry::get_key(a) < Entry::get_key(b);
	};

	// helper function to decide the bucket number and range
	static size_type get_bkt_num(size_type const item_n)
	{
		return item_n / TT::k_buf_md;
	}

	static size_type get_bkt_st_idx(size_type const i, size_type const bkt_n,
									size_type const n)
	{
		return (static_cast<size_t>(n) * i) / bkt_n;
	};

	static size_type get_bkt_end_idx(size_type const i, size_type const bkt_n,
									 size_type const n)
	{
		return (static_cast<size_t>(n) * (i + 1)) / bkt_n;
	};

	// PARA: return whether a node is invalid
	static bool node_has_leaf_children(const Node *T)
	{
		return T->is_leaf ?
				   false :
				   static_cast<const Interior *>(T)->tree_nodes[0]->is_leaf;
	}

	static bool self_balance_node(const Node *T, const bool tomb)
	{
		return node_has_leaf_children(T) && tomb && node_is_valid(T);
	}

	static bool get_node_invalid_status(const Node *T)
	{
		return T->is_leaf ? (T->size < TT::k_buf_lb) :
							(!static_cast<const Interior *>(T)->valid);
	}

	static bool node_is_valid(const Node *T)
	{
		return !get_node_invalid_status(T);
	}

	static bool node_contain_invalid_subtree(const Node *T)
	{
		return T->is_leaf ?
				   (T->size < TT::k_buf_lb ? true : false) :
				   static_cast<const Interior *>(T)->contain_invalid_subtree;
	}

	static size_type get_node_reinsert_size(const Node *T)
	{
		return T->is_leaf ? (T->size < TT::k_buf_lb ? T->size : 0) :
							static_cast<const Interior *>(T)->reinsert_size;
	}

	template <typename Range> void Build_(Range In);
	template <typename Range> Node *build_recursive(Range In);
	Node *assemble_interior_levels(TT::node_slice nodes, TT::split_slice keys);

	template <typename Range, typename LeafSortPred>
		requires std::predicate<LeafSortPred, Node *, Range>
	void BatchInsert_(Range In, const TT::pattern_type leaf_entries_pattern,
					  const bool sorted_input,
					  LeafSortPred const &leaf_sort_pred);

	template <typename Range, typename LeafSortPred>
	size_type
	batch_insert_recursive(Node *T, Range In,
						   node_split_pair_slice node_split_slice,
						   const TT::pattern_type leaf_entries_pattern,
						   LeafSortPred const &leaf_sort_pred);

	template <typename Range, typename LeafSortPred>
		requires std::predicate<LeafSortPred, Node *, Range>
	void BatchDelete_(Range In, const TT::pattern_type leaf_entries_pattern,
					  const bool sorted_input,
					  LeafSortPred const &leaf_sort_pred);

	// Returns (invalid_flag, needs_fetch, reinsert_entry_num).
	// needs_fetch: any subtree needs fetch_invalid_subtree_nodes (not
	// stored in node).
	template <typename LeafSortPred>
	std::tuple<bool, bool, size_type>
	batch_delete_recursive(Node *T, TT::const_key_slice In,
						   const TT::pattern_type leaf_entries_pattern,
						   LeafSortPred const &leaf_sort_pred);

	// Remove the invalid subtree and put it in flatten_node_slice
	// Needs to compact the tree_nodes if find a invalid subtree and update
	// the interior node size and valid flag after the compaction Return the
	// total number entries in the tree that needs to rebuild
	template <typename LeafSortPred>
	void
	fetch_invalid_subtree_nodes(Node *T, entry_slice reinsert_entries,
								bool has_tomb,
								const TT::pattern_type leaf_entries_pattern,
								LeafSortPred const &leaf_sort_pred);

	template <typename LeafSortPred>
	void reinsert_tree_after_delete(Node *T, entry_slice reinsert_entries,
									const TT::pattern_type leaf_entries_pattern,
									LeafSortPred const &leaf_sort_pred);

	void prune_out_empty_tree(Node *T);

	void rebuild_from_flatten();

	template <typename LeafSortPred>
	void balance_over_leaves(Node *T, const size_type reinsert_size,
							 const TT::pattern_type leaf_entries_pattern,
							 LeafSortPred const &leaf_sort_pred);

	static Node *batch_insert_update_interior(
		Node *T, node_split_pair_slice node_split_slice,
		size_buf_ub_arr const &split_pos_arr, size_type new_node_n,
		size_buf_ub_arr const &return_node_n_arr);

	std::tuple<bool, bool, size_type> batch_delete_update_interior_size(
		Node *T, boolean_buf_ub_arr const &invalid_subtree_num_arr,
		boolean_buf_ub_arr const &invalid_subtree_flag_arr,
		size_buf_ub_arr const &reinsert_entries_num_arr);

	// PARA: return bucket_num
	static size_type generate_leaf_nodes(TT::entry_slice key_in,
										 TT::split_slice key_out,
										 TT::node_slice node_out);

	static size_type generate_leaf_nodes(TT::entry_slice key_in,
										 node_split_pair_slice node_split_out);

	// PARA: return bucket_num
	template <typename KeyFunc, typename NodeFunc>
	struct gen_nodes_output_handler {
		KeyFunc set_key;
		NodeFunc set_node;
	};

	template <typename OutputHandler>
	static size_type generate_leaf_nodes(TT::entry_slice key_in,
										 OutputHandler &&output_handler);

	template <typename OutputHandler>
	static size_type generate_interior_nodes(TT::node_slice node_in,
											 TT::split_slice key_in,
											 OutputHandler &&output_handler);

	template <typename Range, typename LeafSortPred>
	static TT::Node *
	post_process_leaf_change(Node *T, Range In,
							 const TT::pattern_type leaf_entries_pattern,
							 LeafSortPred const &leaf_sort_pred);

	// batch insert
	template <typename Range, typename LeafSortPred>
	static Node *
	insert_points_2_leaf(Node *T, Range In,
						 const TT::pattern_type leaf_entries_pattern,
						 LeafSortPred const &leaf_sort_pred);

	static void
	partition_binary_search_rec(auto split_seq_s,
								// parlay::sequence<int>& split_pos_seq,
								auto &split_pos_seq, entry_slice entry_seq_s,
								size_t split_offset, size_t In_offset);

	template <typename Range>
	static size_type
	handling_leaf_split_overflow(Node *T, Range In,
								 node_split_pair_slice node_split_slice);

	static size_type handling_interior_split_overflow(
		Node *T, node_split_pair_slice node_split_slice,
		size_buf_ub_arr const &split_pos_arr, size_type new_node_n,
		size_buf_ub_arr const &return_node_n_arr);

	template <typename LeafSortPred>
	static TT::Node *
	delete_points_4_leaf(Node *T, TT::const_key_slice In,
						 const TT::pattern_type leaf_entries_pattern,
						 LeafSortPred const &leaf_sort_pred);

	// template <typename Leaf, typename RT>

	template <typename Range>
	static void split_batch_binary_search(size_buf_ub_arr &split_pos_arr,
										  Range In,
										  TT::split_slice_const split_slice);

	template <typename Range>
	static void split_batch_merge(size_buf_ub_arr &split_pos_arr, Range In,
								  TT::split_slice_const split_slice);

	// delete tree
	// template <SupportsForceParallel Interior, bool granularity>
	template <bool granularity>
	inline static bool force_parallel_recursion(Interior const *T);

	static Node *copy_tree_recursive(Node const *T);

	template <bool granularity = true> static void delete_tree_rec(Node *T);
	// utility
	// TODO: better evaluate the parallel recursion function
	template <typename Range, bool granularity = true, bool sorted = false>
	static void flatten_rec(Node *T, Range Out);

	template <typename Range, bool sorted = false>
	static void extract_points_in_leaf(Node *T, Range Out);

	// validations

	static TT::Box get_node_box_rec(Node const *T);

	static TT::Box get_node_slice_box(TT::node_slice node_slice);

	static TT::Box get_node_slice_box(TT::const_node_slice node_slice);

	// Returns {subtree box, all checked properties held}.
	template <bool CheckSize = true>
	std::pair<typename TT::Box, bool> verify_tree_property(Node const *T) const;

	bool verify_unoverlapped_tree(Node const *T,
								  TT::node_slice node_slice) const;

	TT::height_type check_height(Node const *T) const;

	bool check_contain_node(Node const *T, Node const *P) const;

	template <typename Leaf, typename Interior> int get_height(Node const *T);

	template <typename Leaf, typename Interior>
	int get_tree_nodes_num(Node const *T);

	// Returns {sorted_leaf_num, total_leaf_num} for the subtree rooted at
	// T.
	static std::pair<size_type, size_type> count_sorted_leaves(Node const *T);

	std::tuple<bool, bool, typename TT::size_type>
	check_tree_aug_after_delete(Node *T);

	bool contain_iter(Node const *T, K const &k) const;

	void range_count_leaf(Node const *T, TT::Box const &query_box,
						  size_type &offset) const;

	void range_count_rec(Node const *T, TT::Box const &query_box,
						 size_type &offset) const;

	template <typename Range>
	void range_report_leaf(Node const *T, TT::Box const &query_box,
						   size_type &offset, Range out) const;

	template <typename Range>
	void range_report_iter(Node const *T, TT::Box const &query_box,
						   size_type &offset, Range out) const;

	template <typename Range>
	void range_report_rec(Node const *T, TT::Box const &query_box,
						  size_type &offset, Range out) const;

	template <typename Range>
	void range_report_flatten_rec(Node const *T, size_type &offset,
								  Range out) const;

public:
	static auto parse_input(TT::entry_seq const &input_seq) -> input_range_type
	{
		return input_seq;
	}

	static auto parse_delete_input(TT::entry_seq const &input_seq)
		-> input_delete_range_type
	{
		return parlay::map(input_seq,
						   [&](auto e) { return Entry::get_key(e); });
	}

	Node *get_root()
	{
		return this->root_;
	}

	Node const *get_root() const
	{
		return this->root_;
	}

	size_t get_size() const
	{
		return this->root_ ? this->root_->size : 0;
	}

	constexpr static char const *get_tree_name()
	{
		return "btree";
	}

	constexpr static int get_leaf_size()
	{
		return TT::k_buf;
	}

	void pab_tree_tag()
	{
	}

private:
	Node *root_ = nullptr;
};

} // namespace pab_tree

#include "pab_tree/b_tree_impl/b_box_op.hpp"
#include "pab_tree/b_tree_impl/batch_op/b_batch_delete.hpp"
#include "pab_tree/b_tree_impl/batch_op/b_batch_insert.hpp"
#include "pab_tree/b_tree_impl/batch_op/b_build_tree.hpp"
#include "pab_tree/b_tree_impl/batch_op/b_delete_tree.hpp"
#include "pab_tree/b_tree_impl/points_op.hpp"
#include "pab_tree/b_tree_impl/query_op/membership.hpp"
#include "pab_tree/b_tree_impl/query_op/range_count.hpp"
#include "pab_tree/b_tree_impl/query_op/range_report.hpp"
#include "pab_tree/b_tree_impl/tree_op/flatten.hpp"
#include "pab_tree/b_tree_impl/tree_op/leaf_op.hpp"
#include "pab_tree/b_tree_impl/tree_op/node_op.hpp"
#include "pab_tree/b_tree_impl/utility.hpp"
#include "pab_tree/b_tree_impl/validation.hpp"

#endif // PABTREE_B_TREE_H_

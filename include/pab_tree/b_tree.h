#ifndef PABTREE_B_TREE_H_
#define PABTREE_B_TREE_H_

#include <array>
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <ostream>
#include <ranges>
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
/* Basetree */
template <class TT>
class btree {
public:
	using type_trait = TT;
	/* TODO: Do we really need to define these types again? */

	using entry_type = typename type_trait::entry_type;
	using key_type = typename type_trait::key_type;
	using mapped_type = typename type_trait::mapped_type;
	using entry_traits = type_trait::entry_traits;

	using node_type = type_trait::node_type;
	using num_comparator = typename type_trait::num_comparator;
	using entry_slice = typename type_trait::entry_slice;
	using split_type = typename type_trait::split_type;
	using node_split_seq_pair = std::pair<typename type_trait::node_seq,
										  typename type_trait::split_seq>;

	using node_split_pair = std::pair<node_type *, split_type>;
	using node_split_pair_seq = parlay::sequence<node_split_pair>;
	using node_split_pair_slice =
		parlay::slice<node_split_pair *, node_split_pair *>;
	using node_seq_list = parlay::sequence<typename type_trait::node_seq>;
	using split_seq_list = parlay::sequence<typename type_trait::split_seq>;

	using leaf_aug = typename type_trait::leaf_aug;
	using interior_aug = typename type_trait::interior_aug;

	using input_range_type = type_trait::entry_seq;
	using input_delete_range_type = type_trait::key_seq;

	using size_buf_ub_arr = typename type_trait::size_buf_ub_arr;
	using buf_size_buf_ub_arr = typename type_trait::buf_size_buf_ub_arr;
	using boolean_buf_ub_arr = typename type_trait::boolean_buf_ub_arr;
	using buf_size_type = type_trait::buf_size_type;
	using size_type = type_trait::size_type;

	/* tree structure */
	static constexpr uint_fast16_t const k_serial_build_cutoff = 512;

	/* constructor and destructor */
	btree() = default;

	/* Constrained so it cannot outrank the copy constructor. */
	template <typename R>
		requires(!std::same_as<std::remove_cvref_t<R>, btree>)
	explicit btree(R &&in)
	{
		this->build(std::forward<R>(in));
	}

	/*
	 * copy constructor
	 * copy @other
	 */
	btree(const btree &other)
		: root_(other.root_ ? this->copy_tree_recursive(other.root_) : nullptr)
	{
	}

	/*
	 * copy assignment operator
	 * copy @other, will launch a deep copy
	 */
	btree &operator=(const btree &other)
	{
		if (this == &other) {
			return *this;
		}

		this->delete_tree(); /* delete current tree */
		this->root_ =
			other.root_ ? this->copy_tree_recursive(other.root_) : nullptr;
		return *this;
	}

	/* move constructor */
	btree(btree &&other) noexcept
		: root_(other.root_)
	{
		other.root_ = nullptr;
	}

	/* move assignment operator */
	btree &operator=(btree &&other) noexcept
	{
		if (this == &other) {
			return *this;
		}

		this->delete_tree(); /* delete current tree */
		this->root_ = other.root_; /* steal */
		other.root_ = nullptr;

		return *this;
	}

	/* destructor */
	~btree()
	{
		this->delete_tree();
	}

	/* PARA: interior node type */
	struct btree_interior_node : multi_way_node<type_trait> {
		using base_node = multi_way_node<type_trait>;

		btree_interior_node(type_trait::node_seq const &_tree_nodes,
							type_trait::split_seq const &_split)
			: base_node(_tree_nodes, _split,
						typename type_trait::interior_aug())
			, valid(true)
			, contain_invalid_subtree(0)
			, reinsert_size(0)
		{
		}

		btree_interior_node(type_trait::node_slice _tree_nodes_slice,
							type_trait::split_slice _split_slice)
			: base_node(_tree_nodes_slice, _split_slice,
						typename type_trait::interior_aug())
			, valid(true)
			, contain_invalid_subtree(0)
			, reinsert_size(0)
		{
		}

		btree_interior_node(type_trait::node_seq const &_tree_nodes,
							type_trait::split_seq const &_split,
							type_trait::interior_aug const &_aug)
			: base_node(_tree_nodes, _split, _aug)
			, valid(true)
			, contain_invalid_subtree(0)
			, reinsert_size(0)
		{
		}

		/* Gated so an empty augmentation compiles to nothing. */
		void update_aug()
		{
			if constexpr (node_has_non_trivial_aug<
							  typename type_trait::interior_aug>) {
				static_assert(
					interior_aug_updatable_with<
						typename type_trait::interior_aug,
						decltype(this->get_node_slice()),
						decltype(this->get_split_slice())>,
					"interior augmentation needs update(children, splits)");
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

	using leaf_type = leaf_node<type_trait>;
	using interior_type = btree_interior_node;

	/* ---- public API ---- */

	/*
	 * STL container type names. key_type / mapped_type come from the
	 * trait; value_type is the stored element, which here is the entry.
	 */
	using value_type = entry_type;
	using difference_type = std::ptrdiff_t;

	/* Order two entries by key, using the trait's comparator. */
	struct value_compare {
		bool operator()(entry_type const &a, entry_type const &b) const
		{
			return type_trait::entry_less(a, b);
		}
	};

	/* The trait's key comparator. */
	using key_compare = typename type_trait::key_compare;

	struct default_hybrid_pred {
		template <typename InputNode, typename R>
		bool operator()(InputNode const *node, R in) const noexcept
		{
			/*
			 * Knuth multiplicative hash over (node_size +
			 * batch_size), XORed with bit 3 of each slice end.
			 */
			auto const v =
				(static_cast<size_t>(node->size) + in.size()) * 2654435761ULL ^
				(reinterpret_cast<uintptr_t>(in.begin()) >> 3) ^
				(reinterpret_cast<uintptr_t>(in.end()) >> 3);
			return v & 1;
		}
	};

	/*
	 * build tree
	 * Keys must be distinct. Duplicates put elements on the wrong side of a
	 * split key, where no descent reaches them; debug builds assert.
	 */
	template <typename R>
	void build(R &&in);

	template <typename R>
	void batch_insert(R &&in);

	template <typename R, typename LeafSortPred = default_hybrid_pred>
	void
	batch_insert(R &&in, const type_trait::pattern_type leaf_entries_pattern,
				 const bool sorted_input,
				 LeafSortPred const &leaf_sort_pred = default_hybrid_pred{});

	template <typename R>
	void batch_delete(R &&in);

	template <typename R, typename LeafSortPred = default_hybrid_pred>
	void
	batch_delete(R &&in, const type_trait::pattern_type leaf_entries_pattern,
				 const bool sorted_input,
				 LeafSortPred const &leaf_sort_pred = default_hybrid_pred{});

	void delete_tree();

	/*
	 * Writes every entry into out, in no particular order. Use to_seq()
	 * for key order.
	 * PRECONDITION: out holds at least size() entries. A smaller out
	 * overruns; debug builds assert.
	 */
	template <typename R>
	void get_entries(R &out) const;

	template <bool CheckSize = true, bool Verbose = true>
	[[nodiscard]] bool validate(node_type const *node = nullptr) const;

	/*
	 * Ratio of sorted leaves to total leaves in the subtree rooted at node
	 * (defaults to the whole tree). Returns 0.0 for an empty tree.
	 */
	double sorted_leaf_ratio(node_type const *node = nullptr) const;

	auto contain(key_type const &k) const -> bool;

	auto range_count(type_trait::box_type query_box) const -> size_type;

	/*
	 * Writes the entries in [first, second) into out and returns how many.
	 * PRECONDITION: out holds at least range_count(query_box) entries. A
	 * smaller out overruns; debug builds assert.
	 */
	template <typename R>
	auto range_report(type_trait::box_type query_box, R &&out) const
		-> size_type;

	/*
	 * ---- STL container interface ----
	 * Names from std::set. build/batch_insert/batch_delete stay the
	 * primary interface; these are the standard spellings of the same
	 * operations.
	 */

	size_type size() const
	{
		return this->root_ ? this->root_->size : 0;
	}

	[[nodiscard]] bool empty() const
	{
		return this->size() == 0;
	}

	size_type max_size() const
	{
		return std::numeric_limits<size_type>::max();
	}

	void clear()
	{
		this->delete_tree();
	}

	bool contains(key_type const &k) const
	{
		return this->contain(k);
	}

	/* Keys are distinct, so this is 0 or 1. */
	size_type count(key_type const &k) const
	{
		return this->contain(k) ? 1 : 0;
	}

	void swap(btree &other) noexcept
	{
		std::swap(this->root_, other.root_);
	}

	key_compare key_comp() const
	{
		return key_compare{};
	}

	value_compare value_comp() const
	{
		return value_compare{};
	}

	/* Same as build. */
	template <typename R>
	void bulk_load(R &&in)
	{
		this->build(std::forward<R>(in));
	}

	/*
	 * Same as batch_insert/batch_delete. Constrained to ranges so a
	 * single entry or key picks the overloads below instead.
	 */
	template <typename R>
		requires std::ranges::range<std::remove_cvref_t<R>>
	void insert(R &&in)
	{
		this->batch_insert(std::forward<R>(in));
	}

	template <typename R>
		requires std::ranges::range<std::remove_cvref_t<R>>
	void erase(R &&in)
	{
		this->batch_delete(std::forward<R>(in));
	}

	/*
	 * Insert or erase one entry. Both go through the batch path, so a
	 * loop over them is much slower than one batch call.
	 */
	void insert(entry_type const &e)
	{
		this->batch_insert(std::array<entry_type, 1>{ e });
	}

	/* Returns whether the key was present. */
	bool erase(key_type const &k)
	{
		if (!this->contain(k)) {
			return false;
		}
		this->batch_delete(std::array<key_type, 1>{ k });
		return true;
	}

	/* Entries in key order. Sorts any unsorted leaf as a side effect. */
	type_trait::entry_seq to_seq() const;

	/*
	 * ---- iteration ----
	 * Leaves are ordered against each other but a leaf's own entries may
	 * not be, so ordered iteration needs the leaf it is on to be sorted.
	 * Every leaf an iterator reaches is sorted on arrival, the same way
	 * get_entries and to_seq already do it.
	 *
	 * That write makes iteration and lookup unsafe to run from several
	 * threads at once. Call sort_leaves first to do the sorting up
	 * front; afterwards both only read.
	 */

	/* Sort every unsorted leaf, in parallel. Idempotent. */
	void sort_leaves() const;

	/*
	 * Bidirectional iterator over entries in key order. Entries are
	 * immutable: moving one would break the leaf it sits in.
	 *
	 * It holds no path stack. Crossing a leaf boundary re-descends from
	 * the root, which is O(height) per leaf and O(height / k_buf) per
	 * entry, and keeps the iterator 3 words wide.
	 */
	class const_iterator {
	public:
		using iterator_category = std::bidirectional_iterator_tag;
		using value_type = entry_type;
		using difference_type = std::ptrdiff_t;
		using pointer = entry_type const *;
		using reference = entry_type const &;

		const_iterator() = default;

		reference operator*() const
		{
			return this->leaf_->elements[this->idx_];
		}

		pointer operator->() const
		{
			return &this->leaf_->elements[this->idx_];
		}

		const_iterator &operator++();

		const_iterator &operator--();

		const_iterator operator++(int)
		{
			auto const tmp = *this;
			++*this;
			return tmp;
		}

		const_iterator operator--(int)
		{
			auto const tmp = *this;
			--*this;
			return tmp;
		}

		bool operator==(const_iterator const &other) const
		{
			return this->leaf_ == other.leaf_ && this->idx_ == other.idx_;
		}

	private:
		friend class btree;

		const_iterator(btree const *tree, leaf_type *leaf, buf_size_type idx)
			: tree_(tree)
			, leaf_(leaf)
			, idx_(idx)
		{
		}

		btree const *tree_ = nullptr;
		/* nullptr marks end(). */
		leaf_type *leaf_ = nullptr;
		buf_size_type idx_ = 0;
	};

	using iterator = const_iterator;
	using reverse_iterator = std::reverse_iterator<const_iterator>;
	using const_reverse_iterator = std::reverse_iterator<const_iterator>;

	const_iterator begin() const;

	const_iterator end() const
	{
		return const_iterator(this, nullptr, 0);
	}

	const_iterator cbegin() const
	{
		return this->begin();
	}

	const_iterator cend() const
	{
		return this->end();
	}

	reverse_iterator rbegin() const
	{
		return reverse_iterator(this->end());
	}

	reverse_iterator rend() const
	{
		return reverse_iterator(this->begin());
	}

	/* end() when the key is absent. */
	const_iterator find(key_type const &k) const;

	/* First entry with key >= k. */
	const_iterator lower_bound(key_type const &k) const;

	/* First entry with key > k. */
	const_iterator upper_bound(key_type const &k) const;

	/* Keys are distinct, so this spans 0 or 1 entries. */
	std::pair<const_iterator, const_iterator>
	equal_range(key_type const &k) const;

	/*
	 * Erase what pos points at, or everything in [first, last). Both
	 * read the keys out first, then run one batch_delete, so no
	 * iterator stays live across the update.
	 */
	void erase(const_iterator pos);

	void erase(const_iterator first, const_iterator last);

	/*
	 * Compare contents. Both sides are flattened in key order, so this
	 * is O(n) work plus O(n) space.
	 */
	bool operator==(btree const &other) const;

	std::weak_ordering operator<=>(btree const &other) const;

	/* ---- implementation ---- */
private:
	/*
	 * leaf_type navigation for const_iterator. All four return nullptr when
	 * there is no such leaf.
	 */
	leaf_type *leftmost_leaf() const;

	leaf_type *rightmost_leaf() const;

	/*
	 * First leaf holding keys > k / last leaf holding keys < k, where k
	 * is a key of the leaf being left.
	 */
	leaf_type *next_leaf(key_type const &k) const;

	leaf_type *prev_leaf(key_type const &k) const;

	/* leaf_type whose key range covers k, nullptr if the tree is empty. */
	leaf_type *leaf_covering(key_type const &k) const;

	/*
	 * Sort a leaf if it is not already. Every leaf handed to an iterator
	 * goes through here.
	 */
	static void ensure_leaf_sorted(leaf_type *leaf);

	static void sort_leaves_rec(node_type *node);

	/* Order by key. Element types are not required to define operator<. */
	static constexpr auto key_less = [](entry_type const &a,
										entry_type const &b) {
		return type_trait::entry_less(a, b);
	};

	/* helper function to decide the bucket number and range */
	static size_type get_bkt_num(size_type const item_n)
	{
		return item_n / type_trait::k_buf_md;
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

	/* PARA: return whether a node is invalid */
	static bool node_has_leaf_children(const node_type *node)
	{
		return node->is_leaf ? false :
							   static_cast<const interior_type *>(node)
								   ->tree_nodes[0]
								   ->is_leaf;
	}

	static bool self_balance_node(const node_type *node, const bool tomb)
	{
		return node_has_leaf_children(node) && tomb && node_is_valid(node);
	}

	static bool get_node_invalid_status(const node_type *node)
	{
		return node->is_leaf ?
				   (node->size < type_trait::k_buf_lb) :
				   (!static_cast<const interior_type *>(node)->valid);
	}

	static bool node_is_valid(const node_type *node)
	{
		return !get_node_invalid_status(node);
	}

	static bool node_contain_invalid_subtree(const node_type *node)
	{
		return node->is_leaf ?
				   (node->size < type_trait::k_buf_lb ? true : false) :
				   static_cast<const interior_type *>(node)
					   ->contain_invalid_subtree;
	}

	static size_type get_node_reinsert_size(const node_type *node)
	{
		return node->is_leaf ?
				   (node->size < type_trait::k_buf_lb ? node->size : 0) :
				   static_cast<const interior_type *>(node)->reinsert_size;
	}

	template <typename R>
	void build_(R in);

	template <typename R>
	node_type *build_recursive(R in);

	node_type *assemble_interior_levels(type_trait::node_slice nodes,
										type_trait::split_slice keys);

	template <typename R, typename LeafSortPred>
		requires std::predicate<LeafSortPred, node_type *, R>
	void
	batch_insert_(R in, const type_trait::pattern_type leaf_entries_pattern,
				  const bool sorted_input, LeafSortPred const &leaf_sort_pred);

	template <typename R, typename LeafSortPred>
	size_type
	batch_insert_recursive(node_type *node, R in,
						   node_split_pair_slice node_split_slice,
						   const type_trait::pattern_type leaf_entries_pattern,
						   LeafSortPred const &leaf_sort_pred);

	template <typename R, typename LeafSortPred>
		requires std::predicate<LeafSortPred, node_type *, R>
	void
	batch_delete_(R in, const type_trait::pattern_type leaf_entries_pattern,
				  const bool sorted_input, LeafSortPred const &leaf_sort_pred);

	/*
	 * Returns (invalid_flag, needs_fetch, reinsert_entry_num).
	 * needs_fetch: any subtree needs fetch_invalid_subtree_nodes (not
	 * stored in node).
	 */
	template <typename LeafSortPred>
	std::tuple<bool, bool, size_type>
	batch_delete_recursive(node_type *node, type_trait::const_key_slice in,
						   const type_trait::pattern_type leaf_entries_pattern,
						   LeafSortPred const &leaf_sort_pred);

	/*
	 * Remove the invalid subtree and put it in flatten_node_slice
	 * Needs to compact the tree_nodes if find a invalid subtree and update
	 * the interior node size and valid flag after the compaction Return the
	 * total number entries in the tree that needs to rebuild
	 */
	template <typename LeafSortPred>
	void fetch_invalid_subtree_nodes(
		node_type *node, entry_slice reinsert_entries, bool has_tomb,
		const type_trait::pattern_type leaf_entries_pattern,
		LeafSortPred const &leaf_sort_pred);

	template <typename LeafSortPred>
	void reinsert_tree_after_delete(
		node_type *node, entry_slice reinsert_entries,
		const type_trait::pattern_type leaf_entries_pattern,
		LeafSortPred const &leaf_sort_pred);

	void prune_out_empty_tree(node_type *node);

	void rebuild_from_flatten();

	template <typename LeafSortPred>
	void
	balance_over_leaves(node_type *node, const size_type reinsert_size,
						const type_trait::pattern_type leaf_entries_pattern,
						LeafSortPred const &leaf_sort_pred);

	static node_type *batch_insert_update_interior(
		node_type *node, node_split_pair_slice node_split_slice,
		size_buf_ub_arr const &split_pos_arr, size_type new_node_n,
		size_buf_ub_arr const &return_node_n_arr);

	std::tuple<bool, bool, size_type> batch_delete_update_interior_size(
		node_type *node, boolean_buf_ub_arr const &invalid_subtree_num_arr,
		boolean_buf_ub_arr const &invalid_subtree_flag_arr,
		size_buf_ub_arr const &reinsert_entries_num_arr);

	/* PARA: return bucket_num */
	static size_type generate_leaf_nodes(type_trait::entry_slice key_in,
										 type_trait::split_slice key_out,
										 type_trait::node_slice node_out);

	static size_type generate_leaf_nodes(type_trait::entry_slice key_in,
										 node_split_pair_slice node_split_out);

	/* PARA: return bucket_num */
	template <typename KeyFunc, typename NodeFunc>
	struct gen_nodes_output_handler {
		KeyFunc set_key;
		NodeFunc set_node;
	};

	template <typename OutputHandler>
	static size_type generate_leaf_nodes(type_trait::entry_slice key_in,
										 OutputHandler &&output_handler);

	template <typename OutputHandler>
	static size_type generate_interior_nodes(type_trait::node_slice node_in,
											 type_trait::split_slice key_in,
											 OutputHandler &&output_handler);

	template <typename R, typename LeafSortPred>
	static type_trait::node_type *post_process_leaf_change(
		node_type *node, R in,
		const type_trait::pattern_type leaf_entries_pattern,
		LeafSortPred const &leaf_sort_pred);

	/* batch insert */
	template <typename R, typename LeafSortPred>
	static node_type *
	insert_points_2_leaf(node_type *node, R in,
						 const type_trait::pattern_type leaf_entries_pattern,
						 LeafSortPred const &leaf_sort_pred);

	static void
	partition_binary_search_rec(auto split_seq_s,
								/* parlay::sequence<int>& split_pos_seq, */
								auto &split_pos_seq, entry_slice entry_seq_s,
								size_t split_offset, size_t in_offset);

	template <typename R>
	static size_type
	handling_leaf_split_overflow(node_type *node, R in,
								 node_split_pair_slice node_split_slice);

	static size_type handling_interior_split_overflow(
		node_type *node, node_split_pair_slice node_split_slice,
		size_buf_ub_arr const &split_pos_arr, size_type new_node_n,
		size_buf_ub_arr const &return_node_n_arr);

	template <typename LeafSortPred>
	static type_trait::node_type *
	delete_points_4_leaf(node_type *node, type_trait::const_key_slice in,
						 const type_trait::pattern_type leaf_entries_pattern,
						 LeafSortPred const &leaf_sort_pred);

	/* template <typename LeafType, typename RT> */

	/*
	 * Partition in across interior's children, picking the cheaper of the two
	 * partitioners. Both batch recursions choose the same way.
	 */
	template <typename R>
	[[gnu::always_inline]] static inline void
	partition_batch(size_buf_ub_arr &split_pos_arr, R in,
					interior_type const *interior);

	template <typename R>
	static void
	split_batch_binary_search(size_buf_ub_arr &split_pos_arr, R in,
							  type_trait::split_slice_const split_slice);

	template <typename R>
	static void split_batch_merge(size_buf_ub_arr &split_pos_arr, R in,
								  type_trait::split_slice_const split_slice);

	/*
	 * delete tree
	 * template <SupportsForceParallel InteriorType, bool Granularity>
	 */
	template <bool Granularity>
	inline static bool force_parallel_recursion(interior_type const *node);

	static node_type *copy_tree_recursive(node_type const *node);

	template <bool Granularity = true>
	static void delete_tree_rec(node_type *node);

	/*
	 * utility
	 * TODO: better evaluate the parallel recursion function
	 */
	template <typename R, bool Granularity = true, bool Sorted = false>
	static void flatten_rec(node_type *node, R out);

	template <typename R, bool Sorted = false>
	static void extract_points_in_leaf(node_type *node, R out);

	/* validations */

	static type_trait::box_type get_node_box_rec(node_type const *node);

	static type_trait::box_type
	get_node_slice_box(type_trait::node_slice node_slice);

	static type_trait::box_type
	get_node_slice_box(type_trait::const_node_slice node_slice);

	/* Returns {subtree box, all checked properties held}. */
	template <bool CheckSize = true>
	std::pair<typename type_trait::box_type, bool>
	verify_tree_property(node_type const *node) const;

	bool verify_unoverlapped_tree(node_type const *node,
								  type_trait::node_slice node_slice) const;

	type_trait::height_type check_height(node_type const *node) const;

	bool check_contain_node(node_type const *node,
							node_type const *target) const;

	template <typename LeafType, typename InteriorType>
	int get_height(node_type const *node);

	template <typename LeafType, typename InteriorType>
	int get_tree_nodes_num(node_type const *node);

	/*
	 * Returns {sorted_leaf_num, total_leaf_num} for the subtree rooted at
	 * node.
	 */
	static std::pair<size_type, size_type>
	count_sorted_leaves(node_type const *node);

	std::tuple<bool, bool, typename type_trait::size_type>
	check_tree_aug_after_delete(node_type *node);

	bool contain_iter(node_type const *node, key_type const &k) const;

	void range_count_leaf(node_type const *node,
						  type_trait::box_type const &query_box,
						  size_type &offset) const;

	void range_count_rec(node_type const *node,
						 type_trait::box_type const &query_box,
						 size_type &offset) const;

	template <typename R>
	void range_report_leaf(node_type const *node,
						   type_trait::box_type const &query_box,
						   size_type &offset, R out) const;

	template <typename R>
	void range_report_iter(node_type const *node,
						   type_trait::box_type const &query_box,
						   size_type &offset, R out) const;

	template <typename R>
	void range_report_rec(node_type const *node,
						  type_trait::box_type const &query_box,
						  size_type &offset, R out) const;

	template <typename R>
	void range_report_flatten_rec(node_type const *node, size_type &offset,
								  R out) const;

public:
	static auto parse_input(type_trait::entry_seq const &input_seq)
		-> input_range_type
	{
		return input_seq;
	}

	static auto parse_delete_input(type_trait::entry_seq const &input_seq)
		-> input_delete_range_type
	{
		return parlay::map(input_seq,
						   [&](auto e) { return entry_traits::get_key(e); });
	}

	node_type *get_root()
	{
		return this->root_;
	}

	node_type const *get_root() const
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
		return type_trait::k_buf;
	}

	void pab_tree_tag()
	{
	}

private:
	node_type *root_ = nullptr;
};

} /* namespace pab_tree */

#include "pab_tree/b_tree_impl/b_box_op.hpp"
#include "pab_tree/b_tree_impl/batch_op/b_batch_delete.hpp"
#include "pab_tree/b_tree_impl/batch_op/b_batch_insert.hpp"
#include "pab_tree/b_tree_impl/batch_op/b_build_tree.hpp"
#include "pab_tree/b_tree_impl/batch_op/b_delete_tree.hpp"
#include "pab_tree/b_tree_impl/points_op.hpp"
#include "pab_tree/b_tree_impl/query_op/iterator.hpp"
#include "pab_tree/b_tree_impl/query_op/membership.hpp"
#include "pab_tree/b_tree_impl/query_op/range_count.hpp"
#include "pab_tree/b_tree_impl/query_op/range_report.hpp"
#include "pab_tree/b_tree_impl/tree_op/flatten.hpp"
#include "pab_tree/b_tree_impl/tree_op/leaf_op.hpp"
#include "pab_tree/b_tree_impl/tree_op/node_op.hpp"
#include "pab_tree/b_tree_impl/utility.hpp"
#include "pab_tree/b_tree_impl/validation.hpp"

#endif /* PABTREE_B_TREE_H_ */

#ifndef PABTREE_B_TREE_IMPL_POINTS_OP_HPP_
#define PABTREE_B_TREE_IMPL_POINTS_OP_HPP_

#include <cassert>

#include <algorithm>
#include <utility>

#include "pab_tree/b_tree.h"

namespace pab_tree
{
template <typename TT>
void btree<TT>::partition_binary_search_rec(
	auto split_seq_s,
	// parlay::sequence<int>& split_pos_seq,
	auto &split_pos_seq, entry_slice entry_seq_s, size_t split_offset,
	size_t in_offset)
{
	if (split_seq_s.size() == 0) {
		return;
	}

	if (split_seq_s.size() == 1) {
		split_pos_seq[split_offset] =
			in_offset +
			parlay::internal::binary_search(entry_seq_s, [&](E const e) {
				return TT::key_less(Entry::get_key(e), split_seq_s[0]);
			});
		return;
	}

	size_t mid = split_seq_s.size() >> 1;
	size_t pos = parlay::internal::binary_search(entry_seq_s, [&](E const e) {
		return TT::key_less(Entry::get_key(e), split_seq_s[mid]);
	});
	split_pos_seq[split_offset + mid] = in_offset + pos;
	partition_binary_search_rec(split_seq_s.cut(0, mid), split_pos_seq,
								entry_seq_s.cut(0, pos), split_offset,
								in_offset);
	partition_binary_search_rec(split_seq_s.cut(mid + 1, split_seq_s.size()),
								split_pos_seq,
								entry_seq_s.cut(pos, entry_seq_s.size()),
								split_offset + mid + 1, in_offset + pos);
}

template <typename TT>
template <typename Range>
void btree<TT>::split_batch_binary_search(size_buf_ub_arr &split_pos_arr,
										  Range In,
										  TT::split_slice_const split_slice)
{
	// TODO: maybe we can decide it by skewed distribution
	size_type offset = 0;
	for (buf_size_type i = 0; i < split_slice.size(); i++) {
		size_type pos = parlay::internal::binary_search(
			In.cut(offset, In.size()), [&](auto e) {
				if constexpr (std::same_as<decltype(e), typename TT::K>) {
					return TT::key_less(e, split_slice[i]);
				} else {
					return TT::key_less(Entry::get_key(e), split_slice[i]);
				}
			});
		offset += pos;
		split_pos_arr[i] = offset;
	}
}

template <typename TT>
template <typename Range>
[[gnu::always_inline]] inline void
btree<TT>::partition_batch(size_buf_ub_arr &split_pos_arr, Range In,
						   Interior const *TI)
{
	// Binary search costs O(split_n log(n/split_n)), the merge O(n +
	// split_n); the merge wins once the batch is small relative to the
	// number of children.
	if (In.size() > TT::k_split_factor * TI->get_split_n()) {
		split_batch_binary_search(split_pos_arr, In, TI->get_split_slice());
	} else {
		split_batch_merge(split_pos_arr, In, TI->get_split_slice());
	}
}

template <typename TT>
template <typename Range>
void btree<TT>::split_batch_merge(size_buf_ub_arr &split_pos_arr, Range In,
								  TT::split_slice_const split_slice)
{
	assert(parlay::is_sorted(
		split_slice, [&](auto k1, auto k2) { return TT::key_less(k1, k2); }));

	size_type cnt = 0;
	auto split_iter = split_slice.begin();
	auto input_iter = In.begin();

	while (split_iter != split_slice.end() && input_iter != In.end()) {
		bool less(false);
		if constexpr (std::same_as<typename Range::value_type,
								   typename TT::K>) {
			less = TT::key_less(*input_iter, *split_iter);
		} else {
			less = TT::key_less(Entry::get_key(*input_iter), *split_iter);
		}
		if (less) {
			++input_iter;
			++cnt;
		} else {
			split_pos_arr[split_iter - split_slice.begin()] = cnt;
			++split_iter;
		}
	}
	while (split_iter != split_slice.end()) {
		split_pos_arr[split_iter - split_slice.begin()] = cnt;
		++split_iter;
	}
}

} // namespace pab_tree

#endif // PABTREE_B_TREE_IMPL_POINTS_OP_HPP_

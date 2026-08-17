#ifndef PBTREE_B_TREE_IMPL_POINTS_OP_HPP_
#define PBTREE_B_TREE_IMPL_POINTS_OP_HPP_

#include <cassert>

#include <algorithm>
#include <utility>

#include "../b_tree.h"

namespace pb_tree {
template<typename TT>
void
BTree<TT>::PartitionBinarySearchRec(auto split_seq_s,
                                    // parlay::sequence<int>& split_pos_seq,
                                    auto& split_pos_seq,
                                    EntrySlice entry_seq_s,
                                    size_t split_offset,
                                    size_t in_offset)
{
  if (split_seq_s.size() == 0) {
    return;
  }

  if (split_seq_s.size() == 1) {
    split_pos_seq[split_offset] =
      in_offset + parlay::internal::binary_search(entry_seq_s, [&](E const e) {
        return Entry::GetKey(e) < split_seq_s[0];
      });
    return;
  }

  size_t mid = split_seq_s.size() >> 1;
  size_t pos = parlay::internal::binary_search(entry_seq_s, [&](E const e) {
    return Entry::GetKey(e) < split_seq_s[mid];
  });
  split_pos_seq[split_offset + mid] = in_offset + pos;
  PartitionBinarySearchRec(split_seq_s.cut(0, mid),
                           split_pos_seq,
                           entry_seq_s.cut(0, pos),
                           split_offset,
                           in_offset);
  PartitionBinarySearchRec(split_seq_s.cut(mid + 1, split_seq_s.size()),
                           split_pos_seq,
                           entry_seq_s.cut(pos, entry_seq_s.size()),
                           split_offset + mid + 1,
                           in_offset + pos);
}

template<typename TT>
template<typename Range>
void
BTree<TT>::SplitBatchBinarySearch(SizeBufUBArr& split_pos_arr,
                                  Range In,
                                  TT::SplitSliceConst split_slice)
{
  // PartitionBinarySearchRec(TI->GetSplitSlice(), split_pos_seq, In, 0, 0);
  // TODO: maybe we can decide it by skewed distribution
  SizeT offset = 0;
  for (BufSizeT i = 0; i < split_slice.size(); i++) {
    SizeT pos =
      parlay::internal::binary_search(In.cut(offset, In.size()), [&](auto e) {
        if constexpr (std::same_as<decltype(e), typename TT::K>) {
          return e < split_slice[i];
        } else {
          return Entry::GetKey(e) < split_slice[i];
        }
      });
    offset += pos;
    split_pos_arr[i] = offset;
  }
}

template<typename TT>
template<typename Range>
void
BTree<TT>::SplitBatchMerge(SizeBufUBArr& split_pos_arr,
                           Range In,
                           TT::SplitSliceConst split_slice)
{
  assert(
    parlay::is_sorted(split_slice, [&](auto k1, auto k2) { return k1 < k2; }));

  SizeT cnt = 0;
  auto split_iter = split_slice.begin();
  auto input_iter = In.begin();

  while (split_iter != split_slice.end() && input_iter != In.end()) {
    bool less(false);
    if constexpr (std::same_as<typename Range::value_type, typename TT::K>) {
      less = *input_iter < *split_iter;
    } else {
      less = Entry::GetKey(*input_iter) < *split_iter;
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

} // namespace pb_tree

#endif // PBTREE_B_TREE_IMPL_POINTS_OP_HPP_

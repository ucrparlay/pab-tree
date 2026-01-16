#ifndef PMOS_B_TREE_IMPL_POINTS_OP_HPP_
#define PMOS_B_TREE_IMPL_POINTS_OP_HPP_

#include <algorithm>
#include <utility>

#include "../b_tree.h"

namespace pmos {
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
typename TT::BucketType
BTree<TT>::RetriveTag(E const e, TT::SplitSliceConst split_slice)
{
  return parlay::internal::binary_search(split_slice, [&](K const key) {
    return key <= Entry::GetKey(e);
  });
}

// NOTE: seieve Points from range A to range B, using the skeleton tags. The
// sums is the number of elemenets within each bucket, the tags_num is the total
// number of buckets in the skeleton
template<typename TT>
typename TT::BallSeq
BTree<TT>::SeievePoints(EntrySlice A,
                        EntrySlice B,
                        TT::SplitSliceConst split_seq)
{
  size_t n = A.size();
  size_t num_block = (n + kBlockSize - 1) >> kLog2Base;
  typename TT::BucketType tags_num = split_seq.size() + 1;
  parlay::sequence<parlay::sequence<SizeT>> offset(
    num_block, parlay::sequence<SizeT>(tags_num));
  assert(offset.size() == num_block && offset[0].size() == tags_num &&
         offset[0][0] == 0);
  parlay::parallel_for(0, num_block, [&](size_t i) {
    for (size_t j = i << kLog2Base; j < std::min((i + 1) << kLog2Base, n);
         j++) {
      auto k = RetriveTag(A[j], split_seq);
      offset[i][k]++;
    }
  });

  auto sums = typename TT::BallSeq(tags_num);
  for (size_t i = 0; i < num_block; i++) {
    auto t = offset[i];
    offset[i] = sums;
    for (BucketType j = 0; j < tags_num; ++j) {
      sums[j] += t[j];
    }
  }

  parlay::parallel_for(0, num_block, [&](size_t i) {
    auto v = parlay::sequence<SizeT>::uninitialized(tags_num);
    size_t tot = 0;
    size_t s_offset = 0;
    for (BucketType k = 0; k < tags_num - 1; ++k) {
      v[k] = tot + offset[i][k];
      tot += sums[k];
      s_offset += offset[i][k];
    }
    v[tags_num - 1] = tot + ((i << kLog2Base) - s_offset);
    for (size_t j = i << kLog2Base; j < std::min((i + 1) << kLog2Base, n);
         j++) {
      auto k = RetriveTag(A[j], split_seq);
      B[v[k]++] = A[j];
    }
  });

  return sums;
}

template<typename TT>
void
BTree<TT>::SplitBatchBinarySearch(SizeBufUBArr& split_pos_arr,
                                  EntrySlice In,
                                  TT::SplitSliceConst split_slice)
{
  // PartitionBinarySearchRec(TI->GetSplitSlice(), split_pos_seq, In, 0, 0);
  // TODO: maybe we can decide it by skewed distribution
  SizeT offset = 0;
  for (BufSizeT i = 0; i < split_slice.size(); i++) {
    SizeT pos = parlay::internal::binary_search(
      In.cut(offset, In.size()), [&](E const e) {
        return Entry::GetKey(e) < split_slice[i];
      });
    offset += pos;
    split_pos_arr[i] = offset;
  }
}

template<typename TT>
void
BTree<TT>::SplitBatchSieve(SizeBufUBArr& split_pos_arr,
                           EntrySlice In,
                           TT::SplitSliceConst split_slice)
{
  SizeT n = In.size();
  SizeT num_block = (n + kBlockSize - 1) >> kLog2Base;
  BufSizeT bkt_n = split_slice.size() + 1;
  parlay::sequence<SizeBufUBArr> offset(num_block, SizeBufUBArr{ 0 });
  assert(offset.size() == num_block && bkt_n < offset[0].size() &&
         offset[0][0] == 0);
  parlay::parallel_for(0, num_block, [&](SizeT i) {
    for (SizeT j = i << kLog2Base; j < std::min((i + 1) << kLog2Base, n); j++) {
      auto k = RetriveTag(In[j], split_slice);
      offset[i][k]++;
    }
  });

  SizeBufUBArr sums{ 0 };
  for (SizeT i = 0; i < num_block; i++) {
    auto t = offset[i];
    offset[i] = sums;
    for (BufSizeT j = 0; j < bkt_n; ++j) {
      sums[j] += t[j];
    }
  }

  auto num = std::accumulate(sums.begin(), sums.begin() + bkt_n, 0);
  if (num != In.size()) {
    std::cout << "num:" << num << ", In.size():" << In.size() << std::endl;
  }
  assert(std::accumulate(sums.begin(), sums.begin() + bkt_n, 0) == In.size());

  SizeT cnt = 0;
  for (BufSizeT i = 0; i < split_slice.size(); i++) {
    cnt += sums[i];
    split_pos_arr[i] = cnt;
  }
}

template<typename TT>
void
BTree<TT>::SplitBatchMerge(SizeBufUBArr& split_pos_arr,
                           EntrySlice In,
                           TT::SplitSliceConst split_slice)
{
  assert(parlay::is_sorted(In, [&](auto e1, auto e2) {
    return Entry::GetKey(e1) < Entry::GetKey(e2);
  }));
  assert(parlay::is_sorted(split_slice, [&](auto k1, auto k2) {
    return k1 < k2;
  }));

  SizeT cnt = 0;
  auto split_iter = split_slice.begin();
  auto input_iter = In.begin();

  while (split_iter != split_slice.end() && input_iter != In.end()) {
    if (Entry::GetKey(*input_iter) < *split_iter) {
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

} // namespace pmos

#endif // PMOS_B_TREE_IMPL_POINTS_OP_HPP_

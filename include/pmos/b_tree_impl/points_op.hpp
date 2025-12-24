#ifndef PMOS_B_TREE_IMPL_POINTS_OP_HPP_
#define PMOS_B_TREE_IMPL_POINTS_OP_HPP_

#include <algorithm>
#include <utility>

#include "../b_tree.h"

namespace pmos {
// template <typename Point, typename DerivedTree, uint_fast8_t kSkHeight,
//           uint_fast8_t kImbaRatio>
// inline void BTree<Point, DerivedTree, kSkHeight, kImbaRatio>::SamplePoints(
//     Slice In, Points& arr) {
//   auto size = arr.size();
//   auto n = In.size();
//   auto indexs = parlay::sequence<uint64_t>::uninitialized(size);
//   for (size_t i = 0; i < size; i++) {
//     indexs[i] = parlay::hash64(i) % n;
//   }
//   std::ranges::sort(indexs);
//   for (size_t i = 0; i < size; i++) {
//     arr[i] = In[indexs[i]];
//   }
//   return;
// }

// template <typename Point, typename DerivedTree, uint_fast8_t kSkHeight,
//           uint_fast8_t kImbaRatio>
// inline typename BTree<Point, DerivedTree, kSkHeight, kImbaRatio>::BucketType
// BTree<Point, DerivedTree, kSkHeight, kImbaRatio>::FindBucket(
//     Point const& p, HyperPlaneSeq const& pivots) {
//   BucketType k(1);
//   while (k <= kPivotNum) {
//     k = k * 2 + 1 -
//         static_cast<BucketType>(
//             Num::Lt(p.pnt[pivots[k].second], pivots[k].first));
//   }
//   assert(pivots[k].first == 0);
//   return pivots[k].second;
// }

// template <typename Point, typename DerivedTree, uint_fast8_t kSkHeight,
//           uint_fast8_t kImbaRatio>
// void BTree<Point, DerivedTree, kSkHeight, kImbaRatio>::Partition(
//     Slice A, Slice B, size_t const n, HyperPlaneSeq const& pivots,
//     parlay::sequence<BallsType>& sums) {
//   size_t num_block = (n + kBlockSize - 1) >> kLog2Base;
//   parlay::sequence<parlay::sequence<BallsType>> offset(
//       num_block, parlay::sequence<BallsType>(kBucketNum));
//   assert(offset.size() == num_block);
//   assert(offset[0].size() == kBucketNum);
//   assert(offset[0][0] == 0);
//   parlay::parallel_for(0, num_block, [&](size_t i) {
//     for (size_t j = i << kLog2Base; j < std::min((i + 1) << kLog2Base, n);
//          j++) {
//       DimsType k = FindBucket(A[j], pivots);
//       offset[i][k]++;
//       /*offset[i][std::move(find_bucket(A[j], pivots))]++;*/
//     }
//   });
//
//   sums = parlay::sequence<BallsType>(kBucketNum);
//   for (size_t i = 0; i < num_block; i++) {
//     auto t = offset[i];
//     offset[i] = sums;
//     for (BucketType j = 0; j < kBucketNum; ++j) {
//       sums[j] += t[j];
//     }
//   }
//
//   parlay::parallel_for(0, num_block, [&](size_t i) {
//     auto v = parlay::sequence<BallsType>::uninitialized(kBucketNum);
//     size_t tot = 0, s_offset = 0;
//     for (BucketType k = 0; k < kBucketNum - 1; ++k) {
//       v[k] = tot + offset[i][k];
//       tot += sums[k];
//       s_offset += offset[i][k];
//     }
//     v[kBucketNum - 1] = tot + ((i << kLog2Base) - s_offset);
//     for (size_t j = i << kLog2Base; j < std::min((i + 1) << kLog2Base, n);
//          j++) {
//       DimsType k = FindBucket(A[j], pivots);
//       // B[v[std::move( find_bucket( A[j], pivots ) )]++] = A[j];
//       B[v[k]++] = A[j];
//     }
//   });
//
//   return;
// }

// template <typename Point, typename DerivedTree, uint_fast8_t kSkHeight,
//           uint_fast8_t kImbaRatio>
// typename BTree<Point, DerivedTree, kSkHeight, kImbaRatio>::PointsIter
// BTree<Point, DerivedTree, kSkHeight, kImbaRatio>::SerialPartition(Slice In,
//                                                                   DimsType d)
//                                                                   {
//   size_t n = In.size();
//   std::ranges::nth_element(In.begin(), In.begin() + n / 2, In.end(),
//                            [&](Point const& p1, Point const& p2) {
//                              return Num::Lt(p1.pnt[d], p2.pnt[d]);
//                            });
//
//   std::ranges::subrange _2ndGroup = std::ranges::partition(
//       In.begin(), In.begin() + n / 2,
//       [&](Point const& p) { return Num::Lt(p.pnt[d], In[n / 2].pnt[d]); });
//
//   if (_2ndGroup.begin() == In.begin()) {  // NOTE: handle duplicated medians
//     _2ndGroup = std::ranges::partition(
//         In.begin() + n / 2, In.end(), [&](Point const& p) {
//           return Num::Eq(p.pnt[d], In[n / 2].pnt[d]);
//         });  // NOTE: now all duplicated median is on the left
//   }
//   return _2ndGroup.begin();
// }

// NOTE: retrive the bucket tag of Point p from the skeleton tags
// template <typename Point, typename DerivedTree, uint_fast8_t kSkHeight,
//           uint_fast8_t kImbaRatio>
// template <IsBinaryNode Interior>
// typename BTree<Point, DerivedTree, kSkHeight, kImbaRatio>::BucketType
// BTree<Point, DerivedTree, kSkHeight, kImbaRatio>::RetriveTag(
//     Point const& p, NodeTagSeq const& tags) {
//   BucketType k(1);
//   while (k <= kPivotNum && (!tags[k].first->is_leaf)) {
//     k = k * 2 + 1 -
//         static_cast<BucketType>(
//             Num::Lt(p.pnt[static_cast<Interior*>(tags[k].first)->split.second],
//                     static_cast<Interior*>(tags[k].first)->split.first));
//   }
//   assert(tags[k].second < kBucketNum);
//   return tags[k].second;
// }
template <typename TT>
void BTree<TT>::PartitionBinarySearchRec(TT::SplitSlice split_seq_s,
                                         parlay::sequence<int>& split_pos_seq,
                                         EntrySlice entry_seq_s,
                                         size_t split_offset,
                                         size_t in_offset) {
  if (split_seq_s.size() == 0)
    return;
  else if (split_seq_s.size() == 1) {
    split_pos_seq[split_offset] =
        in_offset +
        parlay::internal::binary_search(entry_seq_s, [&](E const e) {
          return Entry::GetKey(e) < split_seq_s[0];
        });
    return;
  } else {
    size_t mid = split_seq_s.size() >> 1;
    size_t pos = parlay::internal::binary_search(entry_seq_s, [&](E const e) {
      return Entry::GetKey(e) < split_seq_s[mid];
    });
    split_pos_seq[split_offset + mid] = in_offset + pos;
    PartitionBinarySearchRec(split_seq_s.cut(0, mid), split_pos_seq,
                             entry_seq_s.cut(0, pos), split_offset, in_offset);
    PartitionBinarySearchRec(split_seq_s.cut(mid + 1, split_seq_s.size()),
                             split_pos_seq,
                             entry_seq_s.cut(pos, entry_seq_s.size()),
                             split_offset + mid + 1, in_offset + pos);
    return;
  }
}

template <typename TT>
typename TT::BucketType BTree<TT>::RetriveTag(E const e,
                                              TT::SplitSeq const& split_seq) {
  return parlay::internal::binary_search(
      split_seq, [&](K const key) { return key <= Entry::GetKey(e); });
}

// NOTE: seieve Points from range A to range B, using the skeleton tags. The
// sums is the number of elemenets within each bucket, the tags_num is the total
// number of buckets in the skeleton
template <typename TT>
typename TT::BallSeq BTree<TT>::SeievePoints(EntrySlice A, EntrySlice B,
                                             TT::SplitSeq const& split_seq) {
  size_t n = A.size();
  size_t num_block = (n + kBlockSize - 1) >> kLog2Base;
  typename TT::BucketType tags_num = split_seq.size() + 1;
  parlay::sequence<parlay::sequence<typename TT::BallsType>> offset(
      num_block, parlay::sequence<typename TT::BallsType>(tags_num));
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
    auto v = parlay::sequence<typename TT::BallsType>::uninitialized(tags_num);
    size_t tot = 0, s_offset = 0;
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
}  // namespace pmos

#endif  // PMOS_B_TREE_IMPL_POINTS_OP_HPP_

#ifndef PMOS_DEPENDENCE_TYPE_TRAIT_H_
#define PMOS_DEPENDENCE_TYPE_TRAIT_H_

#include <cstdint>
#include <type_traits>
#include <utility>

#include "comparator.h"
#include "parlay/sequence.h"
#include "parlay/slice.h"

namespace pmos {

template <class EleType>
struct EleWrapper : EleType {
  using E = typename EleType::E;
  using K = typename EleType::K;
  using V = typename EleType::V;
  // using Entry = std::tuple<KeyType, ValType>;

  static inline auto GetKey(E const e) { return EleType::GetKey(e); }
  static inline auto GetVal(E const e) { return EleType::GetVal(e); }
  static inline E ToEntry(K const& k, V const& v) {
    return EleType::ToEntry(k, v);
  };
};

template <typename EleType, uint kBufferSize, typename LeafAugType,
          typename InteriorAugType, uint_fast8_t kSkHeightVal,
          typename NodeType>
class TypeTrait {
 public:
  // NOTE: element type
  using E = EleType::E;
  using K = typename EleType::K;
  using V = typename EleType::V;
  using Entry = EleType;  // NOTE: abstract type to access E

  // NOTE: general type
  using Num = Num_Comparator<K>;
  using EntrySlice = parlay::slice<E*, E*>;
  using ConstEntrySlice = parlay::slice<E const*, E const*>;
  using EntrySeq = parlay::sequence<E>;
  using ConstEntrySeq = parlay::sequence<E> const;
  using EntryIter = typename parlay::sequence<E>::iterator;
  using Box = std::pair<K, K>;
  using BoxSeq = parlay::sequence<Box>;

  using LeafAug = LeafAugType;
  using InteriorAug = InteriorAugType;

  using Node = NodeType;
  using NodeSeq = parlay::sequence<Node*>;
  using NodeSlice = parlay::slice<Node**, Node**>;

  // NOTE: block param in point sieving
  static constexpr uint_fast8_t const kSkeletonHeight = kSkHeightVal;
  // TODO:: needs to check this value as well
  using BucketType =
      std::conditional_t<(kSkeletonHeight > 7), uint_fast16_t, uint_fast8_t>;
  using BallsType = uint_fast32_t;
  static constexpr BucketType const kPivotNum = (1 << kSkeletonHeight) - 1;
  static constexpr BucketType const kBucketNum = 1 << kSkeletonHeight;
  static constexpr uint_fast8_t const kLog2Base = 10;
  static constexpr uint_fast16_t const kBlockSize = 1 << kLog2Base;

  using BucketSeq = parlay::sequence<BucketType>;
  using BallSeq = parlay::sequence<BallsType>;
};

template <typename EleType, uint kBufferSize, typename LeafAugType,
          typename InteriorAugType, uint_fast8_t kSkHeightVal,
          typename NodeType>
class BTreeTypeTrait
    : public TypeTrait<EleWrapper<EleType>, kBufferSize, LeafAugType,
                       InteriorAugType, kSkHeightVal, NodeType> {
 public:
  using BT = TypeTrait<EleWrapper<EleType>, kBufferSize, LeafAugType,
                       InteriorAugType, kSkHeightVal, NodeType>;
  static uint const kBufMDFac = 2;
  static uint const kBufUBFac = 4;
  using BufferSizeType = std::conditional_t<(kBufferSize * kBufUBFac > 255u),
                                            uint_fast16_t, uint_fast8_t>;
  static constexpr BufferSizeType kBuf = kBufferSize;
  static constexpr BufferSizeType kBufLB = kBufferSize;
  static constexpr BufferSizeType kBufMD = kBufMDFac * kBufferSize;
  static constexpr BufferSizeType kBufUB = kBufUBFac * kBufferSize;

  using Split = BT::K;
  using SplitSeq = parlay::sequence<Split>;
  using SplitSlice = parlay::slice<Split*, Split*>;
};

}  // namespace pmos

#endif  // PMOS_DEPENDENCE_TYPE_TRAIT_H_
